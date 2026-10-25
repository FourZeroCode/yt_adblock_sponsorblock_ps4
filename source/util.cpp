#include "util.h"
#include "patch.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include <orbis/libkernel.h>

extern "C" void mh_log(const char* fmt, ...);

#define MH_LOG(fmt, ...) mh_log("[minihook] " fmt "\n", ##__VA_ARGS__)

namespace {

    bool looks_printable_text(const uint8_t* data, size_t len, double min_ratio) {
        if (!data || len == 0) return false;
        size_t printable = 0;
        for (size_t i = 0; i < len; ++i) {
            uint8_t c = data[i];
            if ((c >= 0x20 && c < 0x7F) || c == '\t' || c == '\r' || c == '\n') {
                ++printable;
            }
        }
        return static_cast<double>(printable) / static_cast<double>(len) >= min_ratio;
    }

    std::vector<uint8_t> read_remote_buffer(uint64_t addr, size_t length) {
        std::vector<uint8_t> out(length, 0);
        if (!addr || length == 0 || !is_canonical_address(addr)) {
            return out;
        }
        sys_proc_ro(addr, out.data(), out.size());
        return out;
    }

    bool read_remote_ascii(uint64_t addr, size_t max_bytes, std::string& out) {
        if (!addr || !is_canonical_address(addr) || max_bytes == 0) {
            return false;
        }
        size_t fetch = std::min<size_t>(max_bytes, 2048);
        std::vector<char> buffer(fetch + 1, 0);
        sys_proc_ro(addr, buffer.data(), fetch);
        buffer[fetch] = '\0';

        size_t actual = strnlen(buffer.data(), fetch);
        if (!looks_printable_text(reinterpret_cast<const uint8_t*>(buffer.data()), actual, 0.55)) {
            return false;
        }
        out.assign(buffer.data(), actual);
        return true;
    }


    // Default SponsorBlock categories used when config.json is missing, unreadable,
    // empty, or contains no recognized category IDs. Leave empty to disable all
    // Default hard-coded categories are not used anymore, but the code is left here for future reference.
    const std::vector<std::string> kDefaultCategories = {
        // "sponsor", "intro", "outro", "music_offtopic"
    };
    std::vector<std::string> load_categories(std::string* out_skip_mode) {
        if (out_skip_mode) *out_skip_mode = "auto";
        const char* path = "/data/youtube/config.json";
        int fd = sceKernelOpen(path, 0, 0);
        if (fd < 0) {
            MH_LOG("config.json open failed (%d); using defaults", fd);
            return kDefaultCategories;
        }

        // config.json skipMode and categories, read until EOF.
        std::string content;
        content.reserve(1024);
        char chunk[512];
        bool read_error = false;
        for (;;) {
            size_t n = sceKernelRead(fd, chunk, sizeof(chunk));
            if (n == 0) break; // EOF
            if (n > sizeof(chunk)) {
                read_error = true;
                MH_LOG("config.json read returned invalid size %zu", n);
                break;
            }
            if (content.size() + n > 16384) {
                read_error = true;
                MH_LOG("config.json exceeds 16384 bytes");
                break;
            }
            content.append(chunk, n);
        }
        sceKernelClose(fd);

        if (read_error || content.empty()) {
            MH_LOG("config.json read failed/empty (%zu bytes); using defaults", content.size());
            return kDefaultCategories;
        }
        MH_LOG("config.json read %zu bytes", content.size());

        // The PS4 config is read as raw bytes until EOF;
        if (content.size() >= 3 &&
            static_cast<unsigned char>(content[0]) == 0xEF &&
            static_cast<unsigned char>(content[1]) == 0xBB &&
            static_cast<unsigned char>(content[2]) == 0xBF) {
            content.erase(0, 3);
        }

        // Read skipMode from the config file. Unknown/missing values
        // deliberately default to automatic skipping for backwards compatibility.
        if (out_skip_mode) {
            const size_t key_pos = content.find("\"skipMode\"");
            if (key_pos != std::string::npos) {
                const size_t colon = content.find(':', key_pos + 10);
                if (colon != std::string::npos) {
                    const size_t value_quote = content.find('"', colon + 1);
                    if (value_quote != std::string::npos) {
                        const size_t value_end = content.find('"', value_quote + 1);
                        if (value_end != std::string::npos) {
                            const std::string value = content.substr(value_quote + 1, value_end - value_quote - 1);
                            if (value == "confirm") *out_skip_mode = "confirm";
                            else *out_skip_mode = "auto";
                        }
                    }
                }
            }
            MH_LOG("SponsorBlock skipMode: %s", out_skip_mode->c_str());
        }

        // Match only supported SponsorBlock category IDs. Iterating this unique list
        // also prevents duplicate entries in the resulting JavaScript array.
        const char* known[] = {
          "sponsor", "intro", "outro", "selfpromo", "interaction",
          "music_offtopic", "preview", "filler", "exclusive_access",
          "poi_highlight", "chapter", "hook"
        };

        std::vector<std::string> categories;
        for (const char* candidate : known) {
            if (content.find(candidate) != std::string::npos) {
                categories.push_back(candidate);
            }
        }

        if (categories.empty()) {
            MH_LOG("config.json contains no recognized SponsorBlock category IDs; using defaults");
            return kDefaultCategories;
        }

        MH_LOG("Found %zu recognized category IDs in config.json", categories.size());
        return categories;
    }

    std::string categories_to_js_array(const std::vector<std::string>& categories) {
        std::string result = "[";
        for (size_t i = 0; i < categories.size(); ++i) {
            if (i != 0) result += ", ";
            // Category parser only permits lowercase letters, digits, and underscores.
            result += "'" + categories[i] + "'";
        }
        result += "]";
        return result;
    }

    std::string g_js_payload;
    bool g_js_payload_loaded = false;

}  // namespace

bool is_canonical_address(uint64_t addr) {
    return (addr & 0xFFFF800000000000ull) == 0;
}

void log_preview_bounded(const uint8_t* b, size_t n, size_t max_chars) {
    size_t take = n < max_chars ? n : max_chars;
    char buf[256 + 1];
    if (take > 256) take = 256;
    for (size_t i = 0; i < take; ++i) {
        uint8_t c = b[i];
        buf[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    buf[take] = 0;
    MH_LOG("[execute.preview 0..%zu] %s", take, buf);
}

bool ensure_js_payload_loaded() {
    if (g_js_payload_loaded) {
        return true;
    }

    // Use the embedded JavaScript payload.
    static const char kHardcodedJs[] =
        "(function(){\n"
        "  if (window.__MH_SPONSORBLOCK_LOADED) { return; }\n"
        "  window.__MH_SPONSORBLOCK_LOADED = true;\n"
        "\n"
        "  function showToast(title, subtitle) {\n"
        "    try {\n"
        "      var popupAction = {\n"
        "        openPopupAction: {\n"
        "          popupType: 'TOAST',\n"
        "          popup: {\n"
        "            overlayToastRenderer: {\n"
        "              title: { simpleText: title },\n"
        "              subtitle: { simpleText: subtitle }\n"
        "            }\n"
        "          }\n"
        "        }\n"
        "      };\n"
        "      for (var key in window._yttv) {\n"
        "        if (window._yttv[key] && window._yttv[key].instance && window._yttv[key].instance.resolveCommand) {\n"
        "          window._yttv[key].instance.resolveCommand(popupAction);\n"
        "          break;\n"
        "        }\n"
        "      }\n"
        "    } catch(e) {}\n"
        "  }\n"
        "\n"
        "  var origParse = JSON.parse;\n"
        "  JSON.parse = function() {\n"
        "    var r = origParse.apply(this, arguments);\n"
        "    if (r && typeof r === 'object' && !Array.isArray(r)) {\n"
        "      if (r.adPlacements) { delete r.adPlacements; }\n"
        "      if (r.playerAds)    { delete r.playerAds; }\n"
        "      if (r.adSlots)      { delete r.adSlots; }\n"
        "    }\n"
        "    return r;\n"
        "  };\n"
        "\n"
        "  window.JSON.parse = JSON.parse;\n"
        "  try {\n"
        "    for (var key in window._yttv) {\n"
        "      if (window._yttv[key] && window._yttv[key].JSON && window._yttv[key].JSON.parse) {\n"
        "        window._yttv[key].JSON.parse = JSON.parse;\n"
        "      }\n"
        "    }\n"
        "  } catch(e) {}\n"
        "\n"
        "  var blockedCategories = ['__MH_SPONSORBLOCK_CATEGORIES__'];\n"
        "  var skipMode = '__MH_SPONSORBLOCK_SKIP_MODE__';\n"
        "  if (!Array.isArray(blockedCategories) || blockedCategories.length === 0) {\n"
        "    blockedCategories = [];\n"
        "  }\n"
        "  var sponsorSegments = [];\n"
        "  var currentVideoId = null;\n"
        "  var currentVideo = null;\n"
        "  var skipTimeout = null;\n"
        "  var handledSegments = {};\n"
        "  var lastObservedTime = 0;\n"
        "  var openingRewindGraceUntil = 0;\n"
        "  var activePrompt = null;\n"
        "  var activePromptTimeout = null;\n"
        "\n"
        "  function getVideoId() {\n"
        "    try {\n"
        "      var match = window.location.hash.match(/[?&]v=([^&]+)/);\n"
        "      return match ? match[1] : null;\n"
        "    } catch(e) {\n"
        "      return null;\n"
        "    }\n"
        "  }\n"
        "\n"
        "  function loadSponsorBlock(videoId) {\n"
        "    if (!videoId) return;\n"
        "    var tryPort = function(port) {\n"
        "      try {\n"
        "        var xhr = new XMLHttpRequest();\n"
        "        var url = 'http://127.0.0.1:' + port + '/' + encodeURIComponent(videoId);\n"
        "        xhr.timeout = 2000;\n"
        "        xhr.onload = function() {\n"
        "          if (xhr.status === 200) {\n"
        "            try {\n"
        "              var data = origParse(xhr.responseText);\n"
        "              if (Array.isArray(data) && data.length > 0) {\n"
        "                sponsorSegments = data.filter(function(segment) {\n"
        "                  return blockedCategories.includes(segment.category);\n"
        "                });\n"
        "                scheduleSkip();\n"
        "              } else {\n"
        "                sponsorSegments = [];\n"
        "              }\n"
        "            } catch(e) {}\n"
        "          }\n"
        "        };\n"
        "        xhr.onerror = function() { if (port < 4050) tryPort(port + 1); };\n"
        "        xhr.ontimeout = function() { if (port < 4050) tryPort(port + 1); };\n"
        "        xhr.open('GET', url, true);\n"
        "        xhr.send();\n"
        "      } catch(e) {\n"
        "        if (port < 4050) tryPort(port + 1);\n"
        "      }\n"
        "    };\n"
        "    tryPort(4040);\n"
        "  }\n"
        "\n"
        "  function scheduleSkip() {\n"
        "    // Keep one prompt alive; timeupdate events must not recreate it and reset its timeout.\n"
        "    if (activePrompt) return;\n"
        "    if (skipTimeout) {\n"
        "      clearTimeout(skipTimeout);\n"
        "      skipTimeout = null;\n"
        "    }\n"
        "    if (!currentVideo || currentVideo.paused || sponsorSegments.length === 0) return;\n"
        "    var now = currentVideo.currentTime;\n"
        "    var nextSegments = [];\n"
        "    for (var i = 0; i < sponsorSegments.length; i++) {\n"
        "      var seg = sponsorSegments[i].segment;\n"
        "      var candidate = sponsorSegments[i];\n"
        "      var candidateId = candidate.UUID || (candidate.category + '_' + seg[0] + '_' + seg[1]);\n"
        // Include segments that have already started but have not ended yet.\n"
        "      // This matters for segments at 0s when SponsorBlock data arrives slightly late.\n"
        "      if (!handledSegments[candidateId] && seg[1] > now + 0.05) {\n"
        "        nextSegments.push(candidate);\n"
        "      }\n"
        "    }\n"
        "    nextSegments.sort(function(a, b) { return a.segment[0] - b.segment[0]; });\n"
        "    if (nextSegments.length === 0) return;\n"
        "    var segment = nextSegments[0];\n"
        "    var start = segment.segment[0];\n"
        "    var end = segment.segment[1];\n"
        "    var delay = (start - now) * 1000;\n"
        "    // Let a second L1 press register before an opening auto-skip seeks forward.\n"
        "    if (skipMode === 'auto' && start < 1 && now < 1 && Date.now() < openingRewindGraceUntil) {\n"
        "      delay = Math.max(delay, openingRewindGraceUntil - Date.now());\n"
        "    }\n"
        "    skipTimeout = setTimeout(function() {\n"
        "      if (!currentVideo || currentVideo.paused) return;\n"
        "      var uuid = segment.UUID || (segment.category + '_' + start + '_' + end);\n"
        "      if (handledSegments[uuid]) { scheduleSkip(); return; }\n"
        "      if (skipMode === 'auto') {\n"
        "        // Use the same seek target as confirmation mode, but only report success\n"
        "        // after the browser has accepted the seek.\n"
        "        var videoForSeek = currentVideo;\n"
        "        var targetTime = (isFinite(videoForSeek.duration) && videoForSeek.duration - end < 1) ? Math.max(0, end - 1) : end;\n"
        "        var autoSkipFinished = false;\n"
        "        var finishAutoSkip = function() {\n"
        "          if (autoSkipFinished || videoForSeek !== currentVideo) return;\n"
        "          autoSkipFinished = true;\n"
        "          handledSegments[uuid] = true;\n"
        "          showToast('Segment Skipped', segment.category + ' (' + Math.floor(end - start) + 's)');\n"
        "          videoForSeek.removeEventListener('seeked', finishAutoSkip);\n"
        "          scheduleSkip();\n"
        "        };\n"
        "        try {\n"
        "          videoForSeek.addEventListener('seeked', finishAutoSkip);\n"
        "          videoForSeek.currentTime = targetTime;\n"
        "          if (Math.abs(videoForSeek.currentTime - targetTime) < 0.25 && !videoForSeek.seeking) {\n"
        "            videoForSeek.removeEventListener('seeked', finishAutoSkip);\n"
        "            finishAutoSkip();\n"
        "          } else {\n"
        "            setTimeout(function() {\n"
        "              if (videoForSeek === currentVideo && videoForSeek.currentTime >= targetTime - 0.5) finishAutoSkip();\n"
        "            }, 700);\n"
        "          }\n"
        "        } catch(e) {\n"
        "          videoForSeek.removeEventListener('seeked', finishAutoSkip);\n"
        "          // Seek failed; do not claim the segment was skipped.\n"
        "        }\n"
        "        return;\n"
        "      }\n"
        "      // Confirmation mode: leave playback running and show a temporary prompt.\n"
        "      showSegmentPrompt(segment, uuid, start, end);\n"
        "    }, Math.max(delay, 0));\n"
        "  }\n"
        "\n"
        "  function dismissSegmentPrompt() {\n"
        "    if (activePromptTimeout) { clearTimeout(activePromptTimeout); activePromptTimeout = null; }\n"
        "    if (!activePrompt) return;\n"
        "    if (activePrompt._mhKeyHandler) document.removeEventListener('keydown', activePrompt._mhKeyHandler, true);\n"
        "    if (activePrompt.parentNode) activePrompt.parentNode.removeChild(activePrompt);\n"
        "    activePrompt = null;\n"
        "  }\n"
        "\n"
        "  function markOverlappingSegmentsHandled(start, end) {\n"
        "    var furthestEnd = end;\n"
        "    for (var i = 0; i < sponsorSegments.length; i++) {\n"
        "      var item = sponsorSegments[i];\n"
        "      if (!item || !item.segment || item.segment.length < 2) continue;\n"
        "      var itemStart = item.segment[0];\n"
        "      var itemEnd = item.segment[1];\n"
        "      if (itemStart <= end + 0.05 && itemEnd >= start - 0.05) {\n"
        "        var itemId = item.UUID || (item.category + '_' + itemStart + '_' + itemEnd);\n"
        "        handledSegments[itemId] = true;\n"
        "        if (itemEnd > furthestEnd) furthestEnd = itemEnd;\n"
        "      }\n"
        "    }\n"
        "    return furthestEnd;\n"
        "  }\n"
        "\n"
        "  function resolveSegmentPrompt(segment, uuid, start, end, shouldSkip) {\n"
        "    if (handledSegments[uuid]) { dismissSegmentPrompt(); return; }\n"
        "    var furthestEnd = markOverlappingSegmentsHandled(start, end);\n"
        "    dismissSegmentPrompt();\n"
        "    if (!currentVideo) return;\n"
        "    if (shouldSkip) {\n"
        "      showToast('Segment Skipped', segment.category + ' (' + Math.floor(end - start) + 's)');\n"
        "      try { currentVideo.currentTime = currentVideo.duration - furthestEnd < 1 ? Math.max(0, furthestEnd - 1) : furthestEnd; } catch(e) {}\n"
        "    }\n"
        "    scheduleSkip();\n"
        "  }\n"
        "\n"
        "  function showSegmentPrompt(segment, uuid, start, end) {\n"
        "    dismissSegmentPrompt();\n"
        "    var overlay = document.createElement('div');\n"
        "    overlay.id = 'mh-sponsorblock-confirm';\n"
        "    overlay.style.cssText = 'position:fixed;top:24px;right:24px;z-index:2147483647;display:flex;justify-content:flex-end;pointer-events:none;font-family:Arial,sans-serif;';\n"
        "    var panel = document.createElement('div');\n"
        "    panel.style.cssText = 'pointer-events:auto;min-width:360px;max-width:calc(100vw - 48px);padding:22px 28px;border:1px solid #292929;border-radius:12px;background:rgba(20,20,20,.97);color:#fff;text-align:center;box-shadow:0 4px 24px rgba(0,0,0,.65);';\n"
        "    var title = document.createElement('div');\n"
        "    title.textContent = 'SponsorBlock segment';\n"
        "    title.style.cssText = 'font-size:25px;font-weight:bold;margin-bottom:18px;';\n"
        "    var detail = document.createElement('div');\n"
        "    detail.textContent = segment.category + ' segment (' + Math.max(0, Math.round(end - start)) + ' seconds)';\n"
        "    detail.style.cssText = 'font-size:19px;margin-bottom:18px;';\n"
        "    var buttons = document.createElement('div');\n"
        "    buttons.style.cssText = 'display:flex;justify-content:center;gap:12px;';\n"
        "    var skipButton = document.createElement('button');\n"
        "    skipButton.type = 'button';\n"
        "    skipButton.textContent = 'Skip segment';\n"
        "    skipButton.style.cssText = 'font-size:20px;font-weight:bold;padding:12px 18px;border:2px solid #fff;border-radius:7px;background:#fff;color:#111;';\n"
        "    var keepButton = document.createElement('button');\n"
        "    keepButton.type = 'button';\n"
        "    keepButton.textContent = 'Keep watching';\n"
        "    keepButton.style.cssText = 'font-size:20px;font-weight:bold;padding:12px 18px;border:1px solid #888;border-radius:7px;background:#333;color:#fff;';\n"
        "    var selected = 1;\n"
        "    function updateSelection() {\n"
        "      skipButton.style.background = selected === 1 ? '#fff' : '#333';\n"
        "      skipButton.style.color = selected === 1 ? '#111' : '#fff';\n"
        "      skipButton.style.borderColor = selected === 1 ? '#fff' : '#888';\n"
        "      keepButton.style.background = selected === 0 ? '#fff' : '#333';\n"
        "      keepButton.style.color = selected === 0 ? '#111' : '#fff';\n"
        "      keepButton.style.borderColor = selected === 0 ? '#fff' : '#888';\n"
        "    }\n"
        "    skipButton.onclick = function(event) { if (event) { event.preventDefault(); event.stopPropagation(); } resolveSegmentPrompt(segment, uuid, start, end, true); };\n"
        "    keepButton.onclick = function(event) { if (event) { event.preventDefault(); event.stopPropagation(); } resolveSegmentPrompt(segment, uuid, start, end, false); };\n"
        "    var keyHandler = function(event) {\n"
        "      if (!activePrompt) return;\n"
        "      var key = event.key || '';\n"
        "      var code = event.keyCode || event.which || 0;\n"
        "      if (key === 'ArrowLeft' || key === 'ArrowRight' || code === 37 || code === 39) {\n"
        "        if (event.preventDefault) event.preventDefault();\n"
        "        if (event.stopPropagation) event.stopPropagation();\n"
        "        selected = selected === 0 ? 1 : 0;\n"
        "        updateSelection();\n"
        "        try { (selected === 1 ? skipButton : keepButton).focus(); } catch(e) {}\n"
        "        return;\n"
        "      }\n"
        "      // Circle / Back dismisses only our prompt; prevent YouTube's Back action.\n"
        "      var isBack = key === 'Escape' || key === 'Backspace' || key === 'o' || key === 'O' || code === 27 || code === 8 || code === 79;\n"
        "      if (isBack) {\n"
        "        if (event.preventDefault) event.preventDefault();\n"
        "        if (event.stopPropagation) event.stopPropagation();\n"
        "        if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "        markOverlappingSegmentsHandled(start, end);\n"
        "        dismissSegmentPrompt();\n"
        "        if (activePromptTimeout) { clearTimeout(activePromptTimeout); activePromptTimeout = null; }\n"
        "        scheduleSkip();\n"
        "        return;\n"
        "      }\n"
        "      var activate = key === 'Enter' || key === ' ' || key === 'Spacebar' || key === 'x' || key === 'X' || code === 13 || code === 32 || code === 88;\n"
        "      if (activate) {\n"
        "        if (event.preventDefault) event.preventDefault();\n"
        "        if (event.stopPropagation) event.stopPropagation();\n"
        "        if (event.stopImmediatePropagation) event.stopImmediatePropagation();\n"
        "        if (selected === 1) resolveSegmentPrompt(segment, uuid, start, end, true);\n"
        "        else resolveSegmentPrompt(segment, uuid, start, end, false);\n"
        "      }\n"
        "    };\n"
        "    overlay._mhKeyHandler = keyHandler;\n"
        "    panel.appendChild(title);\n"
        "    panel.appendChild(detail);\n"
        "    panel.appendChild(buttons);\n"
        "    buttons.appendChild(keepButton);\n"
        "    buttons.appendChild(skipButton);\n"
        "    overlay.appendChild(panel);\n"
        "    document.body.appendChild(overlay);\n"
        "    activePrompt = overlay;\n"
        "    document.addEventListener('keydown', keyHandler, true);\n"
        "    updateSelection();\n"
        "    try { skipButton.focus(); } catch(e) {}\n"
        "    // Keep the confirmation prompt visible for the segment's full duration.\n"
        "    activePromptTimeout = setTimeout(function() {\n"
        "      resolveSegmentPrompt(segment, uuid, start, end, false);\n"
        "    }, Math.max(0, (end - start) * 1000));\n"
        "  }\n"
        "\n"
        "  function onScheduleSkip() {\n"
        "    if (currentVideo) {\n"
        "      var now = currentVideo.currentTime;\n"
        "      // Detect an actual backward jump, not every timeupdate near the start.\n"
        "      if (now < lastObservedTime - 1) {\n"
        "        if (now < 1) openingRewindGraceUntil = Date.now() + 1400;\n"
        "        if (sponsorSegments.length) {\n"
        "          for (var i = 0; i < sponsorSegments.length; i++) {\n"
        "            var segment = sponsorSegments[i];\n"
        "            var bounds = segment.segment;\n"
        "            var id = segment.UUID || (segment.category + '_' + bounds[0] + '_' + bounds[1]);\n"
        "            var rewindThreshold = bounds[0] < 1 ? bounds[0] + 0.5 : bounds[0] - 1;\n"
        "            if (now < rewindThreshold) delete handledSegments[id];\n"
        "          }\n"
        "        }\n"
        "      }\n"
        "      lastObservedTime = now;\n"
        "    }\n"
        "    scheduleSkip();\n"
        "  }\n"
        "\n"
        "  function detachVideo() {\n"
        "    dismissSegmentPrompt();\n"
        "    if (currentVideo) {\n"
        "      currentVideo.removeEventListener('play', onScheduleSkip);\n"
        "      currentVideo.removeEventListener('pause', onScheduleSkip);\n"
        "      currentVideo.removeEventListener('timeupdate', onScheduleSkip);\n"
        "      currentVideo = null;\n"
        "    }\n"
        "    if (skipTimeout) {\n"
        "      clearTimeout(skipTimeout);\n"
        "      skipTimeout = null;\n"
        "    }\n"
        "  }\n"
        "\n"
        "  function attachVideo() {\n"
        "    detachVideo();\n"
        "    var video = document.querySelector('video');\n"
        "    if (!video) {\n"
        "      setTimeout(attachVideo, 200);\n"
        "      return;\n"
        "    }\n"
        "    currentVideo = video;\n"
        "    currentVideo.addEventListener('play', onScheduleSkip);\n"
        "    currentVideo.addEventListener('pause', onScheduleSkip);\n"
        "    currentVideo.addEventListener('timeupdate', onScheduleSkip);\n"
        "  }\n"
        "\n"
        "  function onVideoChange() {\n"
        "    var videoId = getVideoId();\n"
        "    if (videoId && videoId !== currentVideoId) {\n"
        "      currentVideoId = videoId;\n"
        "      lastObservedTime = 0;\n"
        "      openingRewindGraceUntil = 0;\n"
        "      sponsorSegments = [];\n"
        "      handledSegments = {};\n"
        "      dismissSegmentPrompt();\n"
        "      attachVideo();\n"
        "      loadSponsorBlock(videoId);\n"
        "    }\n"
        "  }\n"
        "\n"
        "  window.addEventListener('hashchange', onVideoChange, false);\n"
        "  onVideoChange();\n"
        "\n"
        "  setTimeout(function() {\n"
        "    showToast('YT AdBlock + SponsorBlock v1.0.0', 'By FourZeroCode & earthonion');\n"
        "  }, 2000);\n"
        "})();\n";

    g_js_payload.assign(kHardcodedJs, kHardcodedJs + sizeof(kHardcodedJs) - 1);

    // Load categories once and substitute them into the embedded JavaScript.
    std::string skip_mode = "auto";
    std::vector<std::string> categories = load_categories(&skip_mode);
    // Missing, unreadable, empty, or unrecognized config returns kDefaultCategories
    // (empty by default, so SponsorBlock skips no categories).
    if (categories.empty()) {
        MH_LOG("Category loader returned an empty list; forcibly applying defaults");
        categories = kDefaultCategories;
    }
    MH_LOG("Active SponsorBlock categories (%zu): %s",
        categories.size(), categories_to_js_array(categories).c_str());
    // Replace the unique category marker with the configured list. If the marker
    // is absent, the JavaScript fallback remains an empty category list.

    const std::string default_js_list =
        "['__MH_SPONSORBLOCK_CATEGORIES__']";
    const size_t list_pos = g_js_payload.find(default_js_list);
    if (list_pos != std::string::npos) {
        g_js_payload.replace(list_pos, default_js_list.size(),
            categories_to_js_array(categories));
        MH_LOG("Injected SponsorBlock category array into embedded JS");
    }
    else {
        MH_LOG("Could not find category marker in embedded JS; SponsorBlock categories remain empty");
    }

    const std::string mode_marker = "__MH_SPONSORBLOCK_SKIP_MODE__";
    const size_t mode_pos = g_js_payload.find(mode_marker);
    if (mode_pos != std::string::npos) {
        g_js_payload.replace(mode_pos, mode_marker.size(), skip_mode);
        MH_LOG("Injected SponsorBlock skip mode: %s", skip_mode.c_str());
    }
    else {
        MH_LOG("Skip-mode marker missing; JavaScript defaults to automatic mode");
    }

    g_js_payload_loaded = true;
    MH_LOG("[execute.inject] loaded hardcoded payload %zu bytes", g_js_payload.size());
    return true;
}

const std::string& get_js_payload() {
    return g_js_payload;
}

void reset_js_payload_state() {
    g_js_payload.clear();
    g_js_payload_loaded = false;
}

void clear_script_cache() {
    // Empty stub - kept for API compatibility
}

void clear_context_tracking() {
    // Empty stub - kept for API compatibility
}

static uint32_t pattern_to_byte(const char* pattern, uint8_t* bytes) {
    uint32_t count = 0;
    const char* start = pattern;
    const char* end = pattern + strlen(pattern);

    for (const char* current = start; current < end; ++current) {
        if (*current == '?') {
            ++current;
            if (*current == '?') {
                ++current;
            }
            bytes[count++] = 0xff;
        }
        else {
            bytes[count++] = strtoul(current, (char**)&current, 16);
        }
    }
    return count;
}

uint8_t* pattern_scan(uint64_t module_base, uint32_t module_size, const char* signature) {
    if (!module_base || !module_size) {
        return nullptr;
    }

    constexpr uint32_t MAX_PATTERN_LENGTH = 256;
    uint8_t patternBytes[MAX_PATTERN_LENGTH] = { 0 };
    int32_t patternLength = pattern_to_byte(signature, patternBytes);

    if (!patternLength || patternLength >= MAX_PATTERN_LENGTH) {
        MH_LOG("Pattern length too large or invalid! %d", patternLength);
        return nullptr;
    }

    mh_log("[pattern_scan] Scanning module base=0x%lx size=0x%x pattern_len=%d\n",
        module_base, module_size, patternLength);
    mh_log("[pattern_scan] First 16 pattern bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
        patternBytes[0], patternBytes[1], patternBytes[2], patternBytes[3],
        patternBytes[4], patternBytes[5], patternBytes[6], patternBytes[7],
        patternBytes[8], patternBytes[9], patternBytes[10], patternBytes[11],
        patternBytes[12], patternBytes[13], patternBytes[14], patternBytes[15]);

    uint8_t* scanBytes = (uint8_t*)module_base;

    for (uint64_t i = 0; i < module_size; ++i) {
        bool found = true;
        for (int32_t j = 0; j < patternLength; ++j) {
            if (scanBytes[i + j] != patternBytes[j] && patternBytes[j] != 0xff) {
                found = false;
                break;
            }
        }
        if (found) {
            mh_log("[pattern_scan] Match found at offset 0x%lx (addr 0x%lx)\n", i, module_base + i);
            mh_log("[pattern_scan] First 16 bytes at match: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                scanBytes[i], scanBytes[i + 1], scanBytes[i + 2], scanBytes[i + 3],
                scanBytes[i + 4], scanBytes[i + 5], scanBytes[i + 6], scanBytes[i + 7],
                scanBytes[i + 8], scanBytes[i + 9], scanBytes[i + 10], scanBytes[i + 11],
                scanBytes[i + 12], scanBytes[i + 13], scanBytes[i + 14], scanBytes[i + 15]);
            return &scanBytes[i];
        }
    }
    return nullptr;
}

bool try_extract_script_from_candidate(uint64_t candidate, std::string& out,
    StringLikeLayout* out_layout) {
    out.clear();
    if (out_layout) {
        out_layout->valid = false;
    }
    if (!candidate || !is_canonical_address(candidate)) {
        return false;
    }

    constexpr size_t kProbe = 0x100;
    std::array<uint8_t, kProbe> header{};
    sys_proc_ro(candidate, header.data(), header.size());

    auto read_u64 = [&](size_t offset) -> uint64_t {
        if (offset + sizeof(uint64_t) > header.size()) {
            return 0;
        }
        uint64_t value = 0;
        std::memcpy(&value, header.data() + offset, sizeof(value));
        return value;
        };

    auto try_pointer_and_length = [&](size_t d_off, size_t n_off, size_t c_off) -> bool {
        uint64_t data_ptr = read_u64(d_off);
        uint64_t len = read_u64(n_off);
        uint64_t cap = read_u64(c_off);
        if (!data_ptr || !is_canonical_address(data_ptr)) {
            return false;
        }
        if (len == 0 || len > (1ULL << 20)) {
            return false;
        }
        if (cap < len || cap >(1ULL << 20)) {
            return false;
        }
        std::vector<uint8_t> temp = read_remote_buffer(data_ptr, static_cast<size_t>(len));
        if (temp.empty()) {
            return false;
        }
        size_t sample = std::min<size_t>(temp.size(), 512);
        if (!looks_printable_text(temp.data(), sample, 0.55)) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(temp.data()), temp.size());
        if (out_layout) {
            out_layout->valid = true;
            out_layout->data_off = d_off;
            out_layout->size_off = n_off;
            out_layout->cap_off = c_off;
            out_layout->header_span = std::max<size_t>(0x40, std::max({ d_off, n_off, c_off }) + 8);
            MH_LOG("[extract.layout] found at data@0x%zx size@0x%zx cap@0x%zx ptr=%p len=%llu cap=%llu",
                d_off, n_off, c_off, (void*)data_ptr,
                static_cast<unsigned long long>(len), static_cast<unsigned long long>(cap));
        }
        return true;
        };

    // Try common offsets for pointer+size+cap pattern
    for (size_t base = 0; base + 24 <= 0x80; base += 8) {
        if (try_pointer_and_length(base, base + 8, base + 16)) {
            return true;
        }
    }

    // Try additional specific offsets
    const size_t offsets[] = { 0x18, 0x20, 0x28, 0x30, 0x38,
                              0x40, 0x48, 0x50, 0x58, 0x60,
                              0x68, 0x70, 0x78, 0x80, 0x88,
                              0x90, 0x98, 0xa0, 0xa8, 0xb0,
                              0xb8, 0xc0, 0xc8, 0xd0, 0xd8,
                              0xe0, 0xe8, 0xf0 };
    for (size_t off : offsets) {
        if (try_pointer_and_length(off, off + 8, off + 16)) {
            return true;
        }
    }

    // Try fallback: scan for any pointer that points to ASCII
    for (size_t off = 0; off + sizeof(uint64_t) <= header.size(); off += sizeof(uint64_t)) {
        uint64_t maybe_ptr = read_u64(off);
        if (read_remote_ascii(maybe_ptr, 2048, out)) {
            MH_LOG("[extract.fallback] found string via pointer at offset 0x%zx -> %p len=%zu",
                off, (void*)maybe_ptr, out.size());
            if (out_layout && !out.empty()) {
                // Try to infer layout from pointer
                if (off + 24 <= header.size()) {
                    uint64_t maybe_size = read_u64(off + 8);
                    uint64_t maybe_cap = read_u64(off + 16);
                    if (maybe_cap > 0 && maybe_cap <= (1ULL << 20) &&
                        (maybe_size == 0 || (maybe_size <= (1ULL << 20) && maybe_cap >= maybe_size))) {
                        out_layout->valid = true;
                        out_layout->data_off = off;
                        out_layout->size_off = off + 8;
                        out_layout->cap_off = off + 16;
                        out_layout->header_span = std::max<size_t>(0x40, off + 24);
                    }
                }
            }
            return true;
        }
    }

    return false;
}
