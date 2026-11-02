-- NotaTV media controls: info bar during playback and a pause screen with poster and description.
-- Metadata is read from /tmp/notatv-nowplaying.json (written by tvshell before playback).
local mp = require "mp"
local utils = require "mp.utils"

-- translations: the same .po file as tvshell (NOTATV_LANG is set by tvshell; the source text is English)
local TR = {}
do
    local lang = os.getenv("NOTATV_LANG") or "en"
    local f = lang ~= "en" and io.open((os.getenv("TVSHELL_ASSETS") or "/usr/share/tvshell") .. "/lang/" .. lang .. ".po", "r")
    if f then
        local function unq(x)
            x = x:match('^%s*"(.*)"%s*$') or ""
            return (x:gsub("\\(.)", function(c) return c == "n" and "\n" or c == "t" and "\t" or c end))
        end
        local id, str, state, fuzzy = nil, nil, 0, false
        local function flush()
            if id and str and id ~= "" and str ~= "" and not fuzzy then TR[id] = str end
            id, str, state, fuzzy = nil, nil, 0, false
        end
        for line in f:lines() do
            if line:match("^msgid ") then local fz = fuzzy; flush(); fuzzy = fz; id = unq(line:sub(7)); state = 1
            elseif line:match("^msgstr ") then str = unq(line:sub(8)); state = 2
            elseif line:match('^%s*"') then
                if state == 1 then id = id .. unq(line) elseif state == 2 then str = str .. unq(line) end
            elseif line:match("^#,.*fuzzy") then flush(); fuzzy = true
            end
        end
        flush()
        f:close()
    end
end
local function T(x) return TR[x] or x end

local W, H = 1920, 1080
local ov = mp.create_osd_overlay("ass-events")
local ovz = mp.create_osd_overlay("ass-events") -- the browse panel (animated on its own)
ovz.z = 1
local render
local menu = nil       -- {level=0|1, sel=1, section=nil}
local subresults = nil
ov.res_x, ov.res_y = W, H
ovz.res_x, ovz.res_y = W, H
-- only send new content to libass when it changes (redrawing costs fps on the Pi)
local function set_ov(o, data)
    if o.data ~= data then o.data = data; o:update() end
end

local meta = {}
local scrub_to, scrub_step = nil, 0 -- seeking: target time (shown before mpv jumps)
-- smooth seeking: the marker follows the target like a critically damped spring. The speed never changes abruptly, so when
-- the key is held down (LG sends a press about every 0.25 s), the marker moves smoothly instead of jerking.
local SCRUB_W = 9 -- spring stiffness (higher = faster, but more uneven while holding)
local scrub_pos, scrub_v, scrub_last, scrub_timer, scrub_running = nil, 0, 0, nil, false
-- release: LG sends no "released", but while holding a press comes about every 0.25 s. If no more arrive
-- within SCRUB_RELEASE, the key was released: the marker brakes quickly, and the player gets the stop point
local SCRUB_RELEASE = 0.3
local scrub_prev_t, scrub_hold, scrub_frozen = nil, false, false
local function scrub_shown() return scrub_to and (scrub_pos or scrub_to) or nil end
local function scrub_step_anim(now)
    if not scrub_to or not scrub_pos then return false end
    if scrub_hold and not scrub_frozen and scrub_prev_t and now - scrub_prev_t > SCRUB_RELEASE then
        scrub_frozen = true
        local stop = scrub_pos + scrub_v * 0.06 -- a little further ahead, so the braking is soft
        if (scrub_to - scrub_pos) * (scrub_to - stop) < 0 then stop = scrub_to end -- never past the target
        scrub_to = stop
        mp.commandv("script-message", "notatv-scrub-stop", string.format("%.2f", stop))
    end
    local dt = math.min(0.1, now - scrub_last)
    scrub_last = now
    local W = scrub_frozen and 25 or SCRUB_W -- stiff spring when braking: no overshoot
    local n = math.max(1, math.ceil(dt / (1 / 120)))
    local h = dt / n
    for _ = 1, n do
        local a = W * W * (scrub_to - scrub_pos) - 2 * W * scrub_v
        scrub_v = scrub_v + a * h
        scrub_pos = scrub_pos + scrub_v * h
    end
    if math.abs(scrub_to - scrub_pos) < 0.05 and math.abs(scrub_v) < 0.5 then
        scrub_pos, scrub_v = scrub_to, 0
        return false
    end
    return true
end
local visible_until = 0
local poster_shown = false
local timer

-- ---- Twitch chat state and text measuring (also used by the info bar and loading) ----
local chat_on, chat_msgs, chat_dirty = false, {}, false
local CHAT_W, CHAT_PAD, CHAT_FS = 400, 20, 25
local CHAT_RATIO = CHAT_W / W
local CHAT_X = W - CHAT_W
local CHAT_TOP, CHAT_BOT = 24, H - 24
-- approximate character width in Inter (in em), so lines wrap at the panel's edge instead of by character count
local function char_em(ch)
    if #ch > 1 then return 0.62 end -- æøå, emoji etc.
    if ch:match("[iljI.,:;'!|]") then return 0.28 end
    if ch:match("[ftr%(%)%[%] ]") then return 0.36 end
    if ch:match("[mwMW@]") then return 0.86 end
    if ch:match("%u") then return 0.68 end
    if ch:match("%d") then return 0.6 end
    return 0.55
end
local function text_px(s, size)
    local w = 0
    for ch in s:gmatch("[%z\1-\127\194-\244][\128-\191]*") do w = w + char_em(ch) end
    return w * size * 0.86 -- calibrated against a screenshot (Inter is narrower than the table)
end
local function wrap_px(s, maxw, size, maxlines)
    local out, line = {}, ""
    for word in tostring(s or ""):gmatch("%S+") do
        local cand = #line > 0 and (line .. " " .. word) or word
        if #line > 0 and text_px(cand, size) > maxw then
            table.insert(out, line)
            if #out >= maxlines then out[#out] = out[#out] .. " …"; return out end
            line = word
        else
            line = cand
        end
    end
    if #line > 0 then table.insert(out, line) end
    return out
end
-- truncate to a max width in pixels (with "…")
local function fit_px(s, maxw, size)
    s = tostring(s or "")
    if text_px(s, size) <= maxw then return s end
    local out = ""
    for ch in s:gmatch("[%z\1-\127\194-\244][\128-\191]*") do
        if text_px(out .. ch .. "…", size) > maxw then break end
        out = out .. ch
    end
    return out .. "…"
end
-- the width of the area the video fills (the chat takes the right side)
local function video_w() return chat_on and CHAT_X or W end


local function load_meta()
    local f = io.open("/tmp/notatv-nowplaying.json", "r")
    meta = {}
    if not f then return end
    local s = f:read("*a")
    f:close()
    meta = utils.parse_json(s) or {}
end

local function esc(s)
    s = tostring(s or "")
    s = s:gsub("\\", "\\\\"):gsub("{", "\\{"):gsub("}", "\\}"):gsub("\n", " ")
    return s
end

local function fmt(t)
    if not t or t < 0 then return "0:00" end
    t = math.floor(t)
    local h, m, s = math.floor(t / 3600), math.floor(t / 60) % 60, t % 60
    if h > 0 then return string.format("%d:%02d:%02d", h, m, s) end
    return string.format("%d:%02d", m, s)
end

-- wrap text into lines of at most n characters (UTF-8 safe enough for text with æøå)
local function wrap(text, n, maxlines)
    local out, line, count = {}, "", 0
    for word in tostring(text or ""):gmatch("%S+") do
        if #line + #word + 1 > n and #line > 0 then
            table.insert(out, line)
            line = word
            if #out >= maxlines then break end
        else
            line = (#line > 0) and (line .. " " .. word) or word
        end
    end
    if #out < maxlines and #line > 0 then table.insert(out, line)
    elseif #out >= maxlines then out[#out] = out[#out] .. " …" end
    return out
end

-- ---- HDR: when the TV is in HDR (PQ), the OSD is drawn in PQ codes. Without conversion white becomes ~10000 nits.
-- Same conversion as the UI (gfx.c): sRGB -> linear -> BT.2020 -> PQ with SDR white at 203 nits.
local hdr = false
local pq_cache = {}
local function lin(v) v = v / 255; return v <= 0.04045 and v / 12.92 or ((v + 0.055) / 1.055) ^ 2.4 end
local function pq(l)
    if l <= 0 then return 0 end
    local m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    local p = (math.min(l, 1) * 203 / 10000) ^ m1
    return math.floor(((c1 + c2 * p) / (1 + c3 * p)) ^ m2 * 255 + 0.5)
end
-- ASS color (BBGGRR) -> the same color in PQ
local function hc(c)
    if not hdr or not c then return c end
    local hit = pq_cache[c]
    if hit then return hit end
    local b, g, r = tonumber(c:sub(1, 2), 16), tonumber(c:sub(3, 4), 16), tonumber(c:sub(5, 6), 16)
    local R, G, B = lin(r), lin(g), lin(b)
    local r2 = pq(0.6274 * R + 0.3293 * G + 0.0433 * B)
    local g2 = pq(0.0691 * R + 0.9195 * G + 0.0114 * B)
    local b2 = pq(0.0164 * R + 0.0880 * G + 0.8956 * B)
    hit = string.format("%02X%02X%02X", b2, g2, r2)
    pq_cache[c] = hit
    return hit
end

local orig_props = nil
local function update_hdr()
    local g = mp.get_property("video-params/gamma", "")
    -- only when the display actually receives HDR: not when HDR is off (no hint) or the
    -- video is tone-mapped to SDR (target-trc=srgb on a display without HDR)
    local trc = mp.get_property("options/target-trc", "auto")
    local out_hdr = mp.get_property("options/target-colorspace-hint", "yes") ~= "no"
        and (trc == "auto" or trc == "pq" or trc == "hlg")
    local on = (meta.sdr_to_hdr == true) or (out_hdr and (g == "pq" or g == "hlg"))
    if on == hdr then return end
    hdr = on
    local names = {"osd-color", "sub-color", "osd-back-color"}
    if on then
        orig_props = orig_props or {}
        for _, n in ipairs(names) do orig_props[n] = orig_props[n] or mp.get_property(n) end
        local w = string.format("#%02X%02X%02X", pq(1), pq(1), pq(1))
        mp.set_property("osd-color", w)
        mp.set_property("sub-color", w)
    elseif orig_props then
        for n, v in pairs(orig_props) do if v then mp.set_property(n, v) end end
    end
end

local function rect(x, y, w, h, color, alpha)
    color = hc(color)
    return string.format("{\\an7\\pos(0,0)\\bord0\\shad0\\1c&H%s&\\1a&H%s&\\p1}m %d %d l %d %d %d %d %d %d{\\p0}",
        color, alpha, x, y, x + w, y, x + w, y + h, x, y + h)
end

local function text(x, y, size, bold, color, s, align)
    color = hc(color or "FFFFFF")
    return string.format("{\\an%d\\pos(%d,%d)\\fnInter\\fs%d\\b%d\\bord0\\shad0\\1c&H%s&}%s",
        align or 7, x, y, size, bold and 1 or 0, color or "FFFFFF", esc(s))
end

-- overlay-add works in real OSD pixels, everything else is drawn in W x H: scale images when mpv draws at
-- another size (900p for 4K HDR on a display without HDR)
local function overlay_at(id, x, y, file, w, h)
    local d = mp.get_property_native("osd-dimensions")
    local k = (d and d.w and d.w > 0) and d.w / W or 1
    mp.command_native({"overlay-add", id, math.floor(x * k + 0.5), math.floor(y * k + 0.5), file, 0, "bgra", w, h, w * 4,
        math.floor(w * k + 0.5), math.floor(h * k + 0.5)})
end

local function show_poster(x, y)
    if poster_shown or not meta.poster or not meta.poster_w then return end
    local file = (hdr and meta.poster_hdr) or meta.poster
    overlay_at(1, x, y, file, meta.poster_w, meta.poster_h)
    poster_shown = true
end

local function hide_poster()
    if poster_shown then mp.command_native({"overlay-remove", 1}); poster_shown = false end
end

-- ================= Live TV: channel banner and browse panel =================
local chan = nil              -- /tmp/notatv-chan.json
local logo_shown = nil
local zap_start, zap_until = 0, 0
local scroll_t, scroll_d = 0, 0
local anim_timer
local ACCENT, RED, GREY = "F88C81", "3539E5", "B4B4B4"
local ZAP_HOLD, SLIDE, SCROLL = 2.5, 0.22, 0.16

local function load_chan()
    local f = io.open("/tmp/notatv-chan.json", "r")
    if not f then return nil end
    local c = utils.parse_json(f:read("*a"))
    f:close()
    return c
end

local function rrect(x, y, w, h, r, color, alpha, extra)
    r = math.min(r, h / 2, w / 2)
    color = hc(color)
    return string.format("{\\an7\\pos(0,0)\\bord0\\shad0%s\\1c&H%s&\\1a&H%s&\\p1}m %d %d l %d %d b %d %d %d %d %d %d l %d %d b %d %d %d %d %d %d l %d %d b %d %d %d %d %d %d l %d %d b %d %d %d %d %d %d{\\p0}",
        extra or "", color, alpha,
        x + r, y, x + w - r, y, x + w, y, x + w, y, x + w, y + r,
        x + w, y + h - r, x + w, y + h, x + w, y + h, x + w - r, y + h,
        x + r, y + h, x, y + h, x, y + h, x, y + h - r,
        x, y + r, x, y, x, y, x + r, y)
end

local function ease(t) t = math.max(0, math.min(1, t)); return 1 - (1 - t) ^ 3 end

local function cut(s, n)
    s = tostring(s or "")
    if #s <= n then return s end
    local i = n
    while i > 1 and s:byte(i) and s:byte(i) >= 0x80 and s:byte(i) < 0xC0 do i = i - 1 end
    return s:sub(1, i - 1) .. "…"
end

local function hide_logo()
    if logo_shown then mp.command_native({"overlay-remove", 2}); logo_shown = nil end
end

local function show_logo(x, y)
    if not chan or not chan.logo then hide_logo(); return end
    local file = (hdr and chan.logo_hdr) or chan.logo
    local key = file .. ":" .. chan.num
    if logo_shown == key then return end
    overlay_at(2, x, y, file, chan.logo_w, chan.logo_h)
    logo_shown = key
end

local function render_banner(parts, now)
    local c = chan
    table.insert(parts, rect(0, H - 380, W, 380, "000000", "B0"))
    table.insert(parts, rect(0, H - 280, W, 280, "000000", "80"))
    table.insert(parts, rect(0, H - 180, W, 180, "000000", "60"))
    -- logo
    local lx, ly, lw, lh = 120, H - 262, 240, 136
    table.insert(parts, rrect(lx, ly, lw, lh, 18, "F4EEEB", "00"))
    if c.logo then show_logo(lx + math.floor((lw - c.logo_w) / 2), ly + math.floor((lh - c.logo_h) / 2))
    else
        hide_logo()
        table.insert(parts, text(lx + lw / 2, ly + lh / 2, 30, true, "463C3C", cut(c.name, 14), 5))
    end
    -- channel
    local tx = 400
    local num = tostring(c.num or "")
    table.insert(parts, text(tx, H - 262, 32, true, ACCENT, num))
    local px = tx + #num * 19 + 18
    table.insert(parts, rrect(px, H - 258, 78, 32, 16, RED, "00"))
    table.insert(parts, text(px + 39, H - 255, 21, true, "FFFFFF", "LIVE", 8))
    table.insert(parts, text(tx, H - 220, 54, true, "FFFFFF", cut(c.name, 30)))
    local sub = {}
    if c.country then table.insert(sub, c.country) end
    if c.group and c.group ~= "" then table.insert(sub, c.group) end
    if c.total and c.total > 1 then table.insert(sub, string.format("%d af %d", c.pos, c.total)) end
    table.insert(parts, text(tx, H - 146, 28, false, GREY, table.concat(sub, "  ·  ")))
    -- program now/next
    local cx, cw = 1080, W - 120 - 1080
    if c.now then
        table.insert(parts, text(cx, H - 262, 22, true, ACCENT, "NU"))
        table.insert(parts, text(cx + cw, H - 262, 24, false, GREY,
            os.date("%H:%M", c.now_start) .. " - " .. os.date("%H:%M", c.now_stop), 9))
        table.insert(parts, text(cx, H - 228, 38, true, "FFFFFF", cut(c.now, 38)))
        local t = os.time()
        local pct = math.max(0, math.min(1, (t - c.now_start) / math.max(1, c.now_stop - c.now_start)))
        table.insert(parts, rrect(cx, H - 160, cw, 8, 4, "FFFFFF", "B0"))
        if pct > 0 then table.insert(parts, rrect(cx, H - 160, math.max(8, math.floor(cw * pct)), 8, 4, "FFFFFF", "00")) end
        if c.next then
            table.insert(parts, text(cx, H - 136, 26, false, GREY,
                T("Next  ") .. os.date("%H:%M", c.next_start) .. "   " .. cut(c.next, 44)))
        end
    else
        table.insert(parts, text(cx, H - 228, 30, false, GREY, T("No program info")))
    end
    table.insert(parts, text(W - 120, 70, 40, true, "FFFFFF", os.date("%H:%M"), 9))
end

-- browse panel: slides in from the right, the rows slide along with each press
local function render_zap(parts, now)
    local c = chan
    if not c or not c.list or (c.total or 0) < 2 then return false end
    local slide
    if now <= zap_until then slide = 1 - ease((now - zap_start) / SLIDE)
    else slide = ease((now - zap_until) / SLIDE) end
    if now > zap_until + SLIDE then return false end
    local pw, rh, rows = 500, 74, 7
    local px = W - 80 - pw + math.floor(slide * (pw + 120))
    local py, y0 = 130, 130 + 84
    local ph = 84 + rows * rh + 20
    table.insert(parts, rrect(px, py, pw, ph, 28, "1E1A18", "30"))
    table.insert(parts, text(px + 30, py + 26, 26, true, "FFFFFF", string.format(T("Channel %d"), c.pos)))
    table.insert(parts, text(px + pw - 30, py + 28, 24, false, GREY, string.format("af %d", c.total), 9))
    -- highlight (fixed in the middle)
    local hy = y0 + 3 * rh
    table.insert(parts, rrect(px + 14, hy + 4, pw - 40, rh - 8, 16, "FFFFFF", "00"))
    local off = 0
    local st = (now - scroll_t) / SCROLL
    if st < 1 then off = math.floor(scroll_d * rh * (1 - ease(st))) end
    local clip = string.format("\\clip(%d,%d,%d,%d)", px, y0, px + pw, y0 + rows * rh)
    for k, e in ipairs(c.list) do
        local y = y0 + (k - 1) * rh + off
        local center = k == 4 and off == 0
        local col = center and "141414" or "FFFFFF"
        local dist = math.abs(k - 4)
        local a = center and "00" or ({"00", "30", "60", "90"})[dist + 1] or "A0"
        table.insert(parts, string.format("{\\an4\\pos(%d,%d)%s\\fnInter\\fs26\\b1\\bord0\\shad0\\1c&H%s&\\1a&H%s&}%s",
            px + 40, y + rh / 2, clip, hc(center and ACCENT or GREY), a, esc(tostring(e.num))))
        table.insert(parts, string.format("{\\an4\\pos(%d,%d)%s\\fnInter\\fs30\\b%d\\bord0\\shad0\\1c&H%s&\\1a&H%s&}%s",
            px + 120, y + rh / 2, clip, center and 1 or 0, hc(col), a, esc(cut(e.name, 24))))
    end
    -- positionslinje
    local th = rows * rh
    local tb = math.max(28, math.floor(th * math.min(1, rows / c.total)))
    local ty = y0 + math.floor((th - tb) * ((c.pos - 1) / math.max(1, c.total - 1)))
    table.insert(parts, rrect(px + pw - 18, y0, 5, th, 2, "FFFFFF", "C0"))
    table.insert(parts, rrect(px + pw - 18, ty, 5, tb, 2, "FFFFFF", "20"))
    return true
end

local render_menu
function render()
    local paused = mp.get_property_native("pause")
    local now = mp.get_time()
    if menu and not paused then
        local parts = {}
        render_menu(parts)
        ov.data = table.concat(parts, "\n")
        ov:update()
        hide_poster()
        return
    end
    if meta.live and chan and not paused then
        local zp = {}
        local zapping = render_zap(zp, now)
        set_ov(ovz, table.concat(zp, "\n"))
        if not zapping and anim_timer then anim_timer:kill() end
        local banner = {}
        if now <= visible_until then render_banner(banner, now) else hide_logo() end
        set_ov(ov, table.concat(banner, "\n"))
        hide_poster()
        return
    end
    hide_logo()
    set_ov(ovz, "")
    if not paused and now > visible_until then
        ov.data = ""
        ov:update()
        hide_poster()
        return
    end
    local pos = mp.get_property_number("time-pos", 0)
    local dur = mp.get_property_number("duration", 0)
    local live = meta.live or dur <= 0
    local title = meta.title
    if live or not title or title == "" then title = mp.get_property("media-title", "") end
    local parts = {}

    if paused then
        -- pause screen
        table.insert(parts, rect(0, 0, W, H, "000000", "50"))
        local tx = 120
        if meta.poster and meta.poster_w then
            show_poster(120, 160)
            tx = 120 + meta.poster_w + 70
        end
        table.insert(parts, text(tx, 160, 30, true, "BBBBBB", T("PAUSE")))
        table.insert(parts, text(tx, 205, 64, true, "FFFFFF", title))
        if meta.subtitle and meta.subtitle ~= "" then
            table.insert(parts, text(tx, 290, 34, false, "D0D0D0", meta.subtitle))
        end
        local y = 360
        for _, l in ipairs(wrap(meta.description, math.floor((W - tx - 120) / 17), 8)) do
            table.insert(parts, text(tx, y, 30, false, "E6E6E6", l))
            y = y + 42
        end
    else
        hide_poster()
        table.insert(parts, rect(0, H - 300, W, 300, "000000", "70"))
        table.insert(parts, rect(0, H - 200, W, 200, "000000", "50"))
    end

    -- bottom bar: title, subtitle, progress
    local by = H - 250
    if not paused then
        table.insert(parts, text(120, by, 52, true, "FFFFFF", fit_px(title, video_w() - 240, 52)))
        if meta.subtitle and meta.subtitle ~= "" then
            table.insert(parts, text(120, by + 66, 30, false, "D0D0D0", fit_px(meta.subtitle, video_w() - 240, 30)))
        end
    end
    if not live then
        local bar_y, bx, bw = H - 120, 120, W - 240
        local pct = math.max(0, math.min(1, pos / dur))
        table.insert(parts, rect(bx, bar_y, bw, 8, "FFFFFF", "B0"))
        table.insert(parts, rect(bx, bar_y, math.floor(bw * pct), 8, "FFFFFF", "00"))
        table.insert(parts, string.format("{\\an5\\pos(%d,%d)\\bord0\\shad0\\1c&H" .. hc("FFFFFF") .. "&\\p1}m -12 0 b -12 -16 12 -16 12 0 b 12 16 -12 16 -12 0{\\p0}",
            bx + math.floor(bw * pct), bar_y + 4))
        table.insert(parts, text(bx, bar_y + 26, 30, false, "FFFFFF", fmt(pos)))
        local remain = dur - pos
        local ends = os.date("%H:%M", os.time() + math.floor(remain))
        table.insert(parts, text(bx + bw, bar_y + 26, 30, false, "FFFFFF", "-" .. fmt(remain) .. "   ·   " .. string.format(T("Ends at %s"), ends), 9))
        local st = scrub_shown(now)
        if st then -- the target: marker and time above the timeline (slides with each press)
            local tp = math.max(0, math.min(1, st / dur))
            local tx = bx + math.floor(bw * tp)
            table.insert(parts, rect(math.min(tx, bx + math.floor(bw * pct)), bar_y, math.abs(tx - (bx + math.floor(bw * pct))), 8, ACCENT, "40"))
            table.insert(parts, rect(tx - 3, bar_y - 22, 6, 52, ACCENT, "00"))
            local d = st - pos
            local lbl = fmt(st) .. "   (" .. (d >= 0 and "+" or "-") .. fmt(math.abs(d)) .. ")"
            local lx = math.max(bx + 160, math.min(bx + bw - 160, tx))
            table.insert(parts, rrect(lx - 170, bar_y - 92, 340, 58, 29, "1E1A18", "20"))
            table.insert(parts, text(lx, bar_y - 63, 32, true, "FFFFFF", lbl, 5))
        end
    else
        table.insert(parts, text(120, H - 110, 30, true, "FF5555", "● LIVE"))
    end
    -- clock in the corner
    if not menu then table.insert(parts, text(W - 120, 70, 40, true, "FFFFFF", os.date("%H:%M"), 9)) end
    if menu and render_menu then render_menu(parts) end
    ov.data = table.concat(parts, "\n")
    ov:update()
end

local function show(seconds)
    visible_until = mp.get_time() + (seconds or 4)
    render()
end


-- ================= settings menu (the Menu key during playback) =================
local LANG = {da="Danish", dan="Danish", en="English", eng="English", sv="Swedish", swe="Swedish", no="Norwegian", nor="Norwegian",
    nob="Norwegian", de="German", ger="German", deu="German", fr="French", fre="French", fra="French", es="Spanish", spa="Spanish",
    it="Italian", ita="Italian", fi="Finnish", fin="Finnish", nl="Dutch", dut="Dutch", pl="Polish", pol="Polish",
    ar="Arabic", ara="Arabic", tr="Turkish", tur="Turkish", ja="Japanese", jpn="Japanese", ko="Korean", kor="Korean"}


local function chan_label(n)
    if not n then return nil end
    if n == 1 then return "Mono" elseif n == 2 then return "Stereo" elseif n == 6 then return "5.1"
    elseif n == 8 then return "7.1" else return string.format(T("%d channels"), n) end
end

-- a track loaded from a network address (YouTube plays its audio as a separate stream, --audio-file=<URL>):
-- mpv names it after the whole address, hundreds of characters, so neither title nor "external" says anything
local function from_url(t)
    local f = t["external-filename"] or ""
    return f:find("://") ~= nil or (t.title or ""):find("://") ~= nil
end

local function track_label(t)
    local parts = {}
    local url = from_url(t)
    local l = t.lang and (LANG[t.lang] and T(LANG[t.lang]) or t.lang:upper())
    if l then table.insert(parts, l) end
    local title = (not url and t.title and t.title ~= "" and t.title ~= l) and t.title or nil
    if title and #title > 40 then title = title:sub(1, 38):gsub("[\128-\191]+$", ""):gsub("[\192-\255]$", "") .. "…" end
    if title then table.insert(parts, title) end
    if t.type == "audio" then
        local c = chan_label(t["demux-channel-count"])
        if c then table.insert(parts, c) end
        if t.codec then table.insert(parts, t.codec:upper()) end
    elseif t.codec then
        table.insert(parts, t.codec:upper())
    end
    if t.forced then table.insert(parts, T("forced")) end
    if t.external and not url then table.insert(parts, T("external")) end
    if #parts == 0 then return string.format(T("Track %s"), tostring(t.id)) end
    return table.concat(parts, " · ")
end

local function tracks(kind)
    local out = {}
    for _, t in ipairs(mp.get_property_native("track-list") or {}) do
        if t.type == kind then table.insert(out, t) end
    end
    return out
end

local function current_label(kind)
    for _, t in ipairs(tracks(kind)) do if t.selected then return track_label(t) end end
    return kind == "sub" and T("Off") or "-"
end

local ASPECTS = {["-1"] = T("Automatic"), ["16:9"] = "16:9", ["4:3"] = "4:3", ["2.35:1"] = T("2.35:1")}
local stats_on = false
local function sections()
    local s = {}
    table.insert(s, {id = "sub", label = T("Subtitles"), value = current_label("sub")})
    table.insert(s, {id = "audio", label = T("Audio"), value = current_label("audio")})
    if meta.qualities then table.insert(s, {id = "quality", label = T("Quality"), value = meta.quality or "Original"}) end
    if meta.speed then table.insert(s, {id = "speed", label = T("Speed"), value = string.format("%.2gx", mp.get_property_number("speed", 1))}) end
    if meta.subsearch then table.insert(s, {id = "subsearch", label = T("Search subtitles"), value = meta.subnokey and T("Needs a key") or ""}) end
    if meta.channel then table.insert(s, {id = "channel", label = T("Go to channel"), value = meta.channel}) end
    table.insert(s, {id = "subdelay", label = T("Subtitle delay"), value = string.format("%+.1f s", mp.get_property_number("sub-delay", 0))})
    -- used to be on Blue/Yellow; now gathered in the menu (Red = menu everywhere in NotaTV)
    local asp = mp.get_property("video-aspect-override", "-1")
    table.insert(s, {id = "aspect", label = T("Aspect ratio"), value = ASPECTS[asp] or T("Automatic")})
    table.insert(s, {id = "stats", label = T("Technical info"), value = stats_on and T("On") or T("Off")})
    return s
end

local function options(id)
    local o = {}
    if id == "sub" or id == "audio" then
        if id == "sub" then table.insert(o, {label = T("Off"), act = function() mp.set_property("sid", "no") end,
            on = mp.get_property("sid") == "no"}) end
        for _, t in ipairs(tracks(id)) do
            local tid = t.id
            table.insert(o, {label = track_label(t), on = t.selected,
                act = function() mp.set_property_number(id == "sub" and "sid" or "aid", tid) end})
        end
    elseif id == "quality" then
        for _, q in ipairs(meta.qualities) do
            if q:sub(1, 1) == "-" then -- the app says this box cannot play it: shown greyed out, cannot be chosen
                table.insert(o, {label = q:sub(2), off = true})
            else
                table.insert(o, {label = q, on = q == (meta.quality or "Original"),
                    act = function() mp.commandv("script-message", "notatv-quality", q) end})
            end
        end
    elseif id == "speed" then
        for _, v in ipairs({0.5, 0.75, 1, 1.25, 1.5, 2}) do
            table.insert(o, {label = string.format("%.2gx", v), on = math.abs(mp.get_property_number("speed", 1) - v) < 0.01,
                act = function() mp.set_property_number("speed", v) end})
        end
    elseif id == "aspect" then
        local cur = mp.get_property("video-aspect-override", "-1")
        for _, v in ipairs({"-1", "16:9", "4:3", "2.35:1"}) do
            table.insert(o, {label = ASPECTS[v], on = cur == v or (v == "-1" and not ASPECTS[cur]),
                act = function() mp.set_property("video-aspect-override", v) end})
        end
    elseif id == "subdelay" then
        for _, v in ipairs({-2, -1, -0.5, 0, 0.5, 1, 2}) do
            table.insert(o, {label = string.format("%+.1f s", v), on = math.abs(mp.get_property_number("sub-delay", 0) - v) < 0.01,
                act = function() mp.set_property_number("sub-delay", v) end})
        end
    elseif id == "subresults" then
        for i, r in ipairs(subresults or {}) do
            local idx = i - 1
            table.insert(o, {label = r, act = function() mp.commandv("script-message", "notatv-subpick", tostring(idx)) end})
        end
        if #o == 0 then table.insert(o, {label = T("No subtitles found"), act = function() end}) end
    end
    return o
end

local function close_menu()
    menu = nil
    mp.commandv("script-message", "notatv-menu", "0")
    render()
end

local function open_menu()
    menu = {level = 0, sel = 1}
    mp.commandv("script-message", "notatv-menu", "1")
    render()
end

function render_menu(parts)
    if not menu then return end
    local x, w = W - 640, 600
    table.insert(parts, rect(x - 20, 0, w + 60, H, "101318", "18"))
    local list, title
    if menu.level == 0 then
        list = sections()
        title = T("Settings")
    else
        list = options(menu.section)
        title = menu.title
    end
    menu.count = #list
    if menu.sel > #list then menu.sel = #list end
    table.insert(parts, text(x + 30, 70, 44, true, "FFFFFF", title))
    local top = 160
    local maxrows = 11
    local first = math.max(1, menu.sel - maxrows + 1)
    for i = first, math.min(#list, first + maxrows - 1) do
        local it = list[i]
        local y = top + (i - first) * 76
        local f = i == menu.sel
        if f then table.insert(parts, rect(x + 10, y, w, 64, "FFFFFF", "00")) end
        local col = it.off and (f and "707070" or "6A6A6A") or (f and "101010" or "FFFFFF")
        local mark = (menu.level == 1 and it.on) and "✓ " or "   "
        table.insert(parts, text(x + 30, y + 14, 30, f, col, mark .. it.label))
        if menu.level == 0 and it.value and it.value ~= "" then
            table.insert(parts, text(x + w - 20, y + 17, 26, false, f and "404040" or "B0B0B0", it.value, 9))
        end
    end
    table.insert(parts, text(x + 30, H - 70, 24, false, "909090", T("OK: select   ·   Back: close")))
end

local function menu_key(k)
    if not menu then return end
    if k == "up" then menu.sel = math.max(1, menu.sel - 1)
    elseif k == "down" then menu.sel = math.min(menu.count or 1, menu.sel + 1)
    elseif k == "back" or (k == "left" and menu.level == 0) then
        if menu.level == 1 and menu.section ~= "subresults" then menu.level = 0; menu.sel = menu.ret or 1
        else close_menu(); return end
    elseif k == "left" then menu.level = 0; menu.sel = menu.ret or 1
    elseif k == "ok" or k == "right" then
        if menu.level == 0 then
            local sec = sections()[menu.sel]
            if not sec then return end
            if sec.id == "subsearch" and meta.subnokey then
                -- no search without a key: say where to add it (two short lines, so it fits on the screen)
                mp.osd_message(T("Searching for subtitles needs a free OpenSubtitles key.") .. "\n" ..
                    T("Add it in Settings → Picture and sound."), 6)
                close_menu()
                return
            elseif sec.id == "subsearch" then
                mp.commandv("script-message", "notatv-subsearch")
                mp.osd_message(T("Searching for subtitles…"), 3)
                close_menu()
                return
            elseif sec.id == "stats" then
                stats_on = not stats_on
                mp.command("script-binding stats/display-stats-toggle")
                close_menu()
                return
            elseif sec.id == "channel" then
                mp.commandv("script-message", "notatv-channel")
                close_menu()
                return
            end
            menu.ret = menu.sel
            menu.section, menu.title, menu.level, menu.sel = sec.id, sec.label, 1, 1
            for i, o in ipairs(options(sec.id)) do if o.on then menu.sel = i end end
        elseif k == "ok" then
            local o = options(menu.section)[menu.sel]
            if o and o.off then
                mp.osd_message(T("This box cannot play that quality"), 3)
                render()
                return
            end
            if o then o.act() end
            if menu.section == "quality" or menu.section == "subresults" then close_menu(); return end
            menu.level = 0
            menu.sel = menu.ret or 1
        end
    end
    render()
end

mp.register_script_message("notatv-menu-open", function() if menu then close_menu() else open_menu() end end)
mp.register_script_message("notatv-key", menu_key)
mp.register_script_message("notatv-subresults", function()
    local f = io.open("/tmp/notatv-subs.json", "r")
    subresults = {}
    if f then subresults = utils.parse_json(f:read("*a")) or {}; f:close() end
    menu = {level = 1, sel = 1, section = "subresults", title = T("Subtitles found")}
    mp.commandv("script-message", "notatv-menu", "1")
    render()
end)

mp.register_script_message("notatv-show", function(sec) show(tonumber(sec) or 4) end)
-- the Audio/Subtitles keys: tvshell cycles the track, this shows it with the same names as the menu
mp.register_script_message("notatv-track-osd", function(kind)
    mp.osd_message((kind == "sub" and T("Subtitles: %s") or T("Audio: %s")):format(current_label(kind)), 2.5)
end)

-- ---- fade from/to black on start and stop (the UI fades out/in to match) ----
local ovk = mp.create_osd_overlay("ass-events")
ovk.z = 20
ovk.res_x, ovk.res_y = W, H
local fade_dir, fade_t0, fade_timer = nil, 0, nil
local status_leave -- set by the status layer further down
local function fade_draw()
    local k = math.min(1, (mp.get_time() - fade_t0) / 0.25)
    local a = fade_dir == "out" and k or 1 - k
    if fade_dir == "in" and k >= 1 then
        ovk.data = ""; ovk:update(); fade_timer:kill(); return
    end
    ovk.data = rect(0, 0, W, H, "000000", string.format("%02X", math.floor((1 - a) * 255)))
    ovk:update()
    if fade_dir == "out" and k >= 1 then fade_timer:kill() end
end
fade_timer = mp.add_periodic_timer(1 / 30, fade_draw)
fade_timer:kill()
local faded_in = false
mp.register_script_message("notatv-fadeout", function()
    if status_leave then status_leave() end
    fade_dir, fade_t0 = "out", mp.get_time()
    fade_timer:resume()
    fade_draw()
end)
mp.register_event("playback-restart", function()
    if faded_in then return end -- only on start (not on seeking/channel change)
    faded_in = true
    fade_dir, fade_t0 = "in", mp.get_time()
    fade_timer:resume()
    fade_draw()
end)
-- ---- live channel change: picture and sound fade to black, the new channel fades in when it plays ----
-- (below the banner, browse panel and loading spinner, so they stay visible on the black)
local ovb = mp.create_osd_overlay("ass-events")
ovb.z = -1
ovb.res_x, ovb.res_y = W, H
local ZF_OUT, ZF_IN = 0.15, 0.35
local zf_a, zf_dir, zf_last, zf_vol, zf_wait = 0, nil, 0, nil, false
local zf_timer
local function zf_step()
    local now = mp.get_time()
    local dt = now - zf_last
    zf_last = now
    if zf_dir == "out" then zf_a = math.min(1, zf_a + dt / ZF_OUT)
    elseif zf_dir == "in" then zf_a = math.max(0, zf_a - dt / ZF_IN) end
    ovb.data = zf_a > 0 and rect(0, 0, W, H, "000000", string.format("%02X", math.floor((1 - zf_a) * 255))) or ""
    ovb:update()
    if zf_vol then mp.set_property_number("volume", zf_vol * (1 - zf_a)) end
    if zf_dir == "in" and zf_a <= 0 then
        if zf_vol then mp.set_property_number("volume", zf_vol); zf_vol = nil end
        zf_dir = nil
        zf_timer:kill()
    elseif zf_dir == "out" and zf_a >= 1 then
        zf_timer:kill()
    end
end
zf_timer = mp.add_periodic_timer(1 / 30, zf_step)
zf_timer:kill()
local function zf_out()
    if not zf_vol then zf_vol = mp.get_property_number("volume", 100) end
    if zf_dir ~= "out" then zf_dir, zf_last = "out", mp.get_time() end
    zf_timer:resume()
    zf_step()
end
-- tvshell: the user stopped browsing - fade out while the old channel still plays
mp.register_script_message("notatv-zapfade", zf_out)
-- a new press during the fade-out: back to the picture (unless a channel is already loading)
mp.register_script_message("notatv-zapcancel", function()
    if zf_dir == "out" and not zf_wait then
        zf_dir, zf_last = "in", mp.get_time()
        zf_timer:resume()
    end
end)
mp.register_script_message("notatv-zap", function(idx)
    zf_out()
    zf_wait = true
    mp.commandv("playlist-play-index", idx)
end)

mp.register_event("playback-restart", function()
    if not zf_wait then return end
    zf_wait = false
    zf_dir, zf_last = "in", mp.get_time()
    zf_timer:resume()
end)
mp.register_script_message("notatv-scrub", function(t, step)
    local now = mp.get_time()
    local nt = tonumber(t)
    -- continue from what is shown right now (at the same speed), otherwise start from the playback position
    if nt then
        if not scrub_to or not scrub_pos then
            scrub_pos, scrub_v = mp.get_property_number("time-pos", nt) or nt, 0
        end
        if not scrub_running then scrub_last = now end -- if the spring is already running, its time must not be reset
        scrub_hold = scrub_prev_t ~= nil and now - scrub_prev_t < 0.45 -- presses close together = the key is held
        scrub_prev_t, scrub_frozen = now, false
    else
        scrub_prev_t, scrub_hold, scrub_frozen = nil, false, false
        scrub_pos, scrub_v = nil, 0
    end
    scrub_to = nt
    scrub_step = tonumber(step) or 0
    if scrub_to and scrub_timer and not scrub_running then scrub_running = true; scrub_timer:resume() end
    show(scrub_to and 6 or 3)
end)
mp.register_script_message("notatv-chan", function(mode)
    local now = mp.get_time()
    local old = chan
    chan = load_chan() or chan
    if mode == "zap" then
        if now > zap_until + SLIDE then zap_start = now end
        local d = 0
        if old and chan and old.pos and chan.pos and chan.total then
            d = chan.pos - old.pos
            if d > chan.total / 2 then d = d - chan.total elseif d < -chan.total / 2 then d = d + chan.total end
            d = math.max(-1, math.min(1, d))
        end
        scroll_d, scroll_t = d, now
        zap_until = now + ZAP_HOLD
        visible_until = now + ZAP_HOLD + 2
        anim_timer:resume()
    elseif mode ~= "update" then
        visible_until = math.max(visible_until, now + 6)
    end
    render()
end)
anim_timer = mp.add_periodic_timer(1 / 25, function() render() end)
scrub_timer = mp.add_periodic_timer(1 / 60, function()
    local moving = scrub_step_anim(mp.get_time())
    render()
    if not moving then scrub_running = false; scrub_timer:kill() end
end)
scrub_timer:kill()

-- ---- Twitch chat: sidebar in the right quarter; the video is scaled down next to it (yellow toggles) ----
local ovc = mp.create_osd_overlay("ass-events")
ovc.z = 5
ovc.res_x, ovc.res_y = W, H
local function chat_draw()
    chat_dirty = false
    if not chat_on or menu or mp.get_property_native("pause") then set_ov(ovc, ""); return end
    local parts, y, blocks = {}, CHAT_BOT - 8, {}
    for i = #chat_msgs, 1, -1 do -- newest at the bottom, build upward
        local m = chat_msgs[i]
        local lines = wrap_px(m.name .. ": " .. m.text, CHAT_W - CHAT_PAD - 4, CHAT_FS, 5) -- right: all the way to the edge
        local hgt = #lines * 32 + 10
        if y - hgt < CHAT_TOP + 60 then break end
        y = y - hgt
        table.insert(blocks, 1, {m = m, lines = lines, y = y})
    end
    table.insert(parts, rect(CHAT_X, 0, CHAT_W, H, "141414", "00"))
    table.insert(parts, rect(CHAT_X, 0, 2, H, "2A2A2A", "00"))
    table.insert(parts, text(CHAT_X + CHAT_PAD, CHAT_TOP, 26, true, "BBBBBB", "CHAT"))
    if #blocks == 0 then table.insert(parts, text(CHAT_X + CHAT_PAD, CHAT_TOP + 60, 24, false, "777777", T("Waiting for messages…"))) end
    for _, b in ipairs(blocks) do
        for li, l in ipairs(b.lines) do
            local ly = b.y + (li - 1) * 32
            if li == 1 and l:sub(1, #b.m.name + 1) == b.m.name .. ":" then
                table.insert(parts, string.format("{\\an7\\pos(%d,%d)\\fnInter\\fs25\\bord0\\shad0\\b1\\1c&H%s&}%s{\\b0\\1c&H%s&}%s",
                    CHAT_X + CHAT_PAD, ly, hc(b.m.color), esc(b.m.name), hc("FFFFFF"), esc(l:sub(#b.m.name + 1))))
            else
                table.insert(parts, text(CHAT_X + CHAT_PAD, ly, CHAT_FS, false, "FFFFFF", l))
            end
        end
    end
    set_ov(ovc, table.concat(parts, "\n"))
end
mp.register_script_message("notatv-chat", function(s)
    local color, name, txt = s:match("^(%x%x%x%x%x%x)\t([^\t]*)\t(.*)$")
    if not color then return end
    table.insert(chat_msgs, {color = color, name = name, text = txt})
    while #chat_msgs > 40 do table.remove(chat_msgs, 1) end
    chat_dirty = true
end)
mp.register_script_message("notatv-chat-show", function(v)
    local on = v == "1"
    if on == chat_on then return end
    chat_on = on
    if not on then chat_msgs = {} end -- new connection next time: start over
    mp.set_property_number("video-margin-ratio-right", on and CHAT_RATIO or 0) -- the video next to the chat
    chat_draw()
end)
-- draw at most 4 times per second (libass costs fps on the Pi; busy chats send many messages)
local chat_menu = false
mp.add_periodic_timer(0.25, function()
    local m = menu ~= nil -- the playback menu covers the same side: hide the chat meanwhile
    if chat_dirty or (chat_on and m ~= chat_menu) then chat_menu = m; chat_draw() end
end)
mp.observe_property("pause", "bool", function() if chat_on then chat_draw() end end)

-- ---- button: "Skip intro" / "Skip credits" / "Next episode" (OK presses it; tvshell decides when it is shown) ----
local ovp = mp.create_osd_overlay("ass-events")
ovp.z = 12
ovp.res_x, ovp.res_y = W, H
mp.register_script_message("notatv-prompt", function(l1, l2)
    if not l1 or l1 == "" then set_ov(ovp, ""); return end
    local parts = {}
    local s1 = 34
    local t2 = (l2 and l2 ~= "") and fit_px(l2, 520, 26) or nil
    local w = math.max(text_px(l1, s1), t2 and text_px(t2, 26) or 0) + 150
    local h = t2 and 108 or 78
    local x, y = W - 80 - w, H - 300
    table.insert(parts, rrect(x, y, w, h, 14, "F2F2F2", "10"))
    table.insert(parts, rrect(x + 20, y + h / 2 - 20, 58, 40, 10, "1A1A1A", "00"))
    table.insert(parts, text(x + 49, y + h / 2, 22, true, "FFFFFF", "OK", 5))
    local ty = t2 and y + 18 or y + h / 2 - 20
    table.insert(parts, text(x + 98, ty, s1, true, "111111", l1, 7))
    if t2 then table.insert(parts, text(x + 98, y + 62, 26, false, "555555", t2, 7)) end
    set_ov(ovp, table.concat(parts, "\n"))
end)
mp.register_event("end-file", function() set_ov(ovp, "") end)

-- ---- status: loading, buffering, seeking and pause/play icon (all sources) ----
local ovs = mp.create_osd_overlay("ass-events")
ovs.z = 30
ovs.res_x, ovs.res_y = W, H
local started, seek_t, flash, flash_t, leaving = false, nil, nil, 0, false
local status_timer

local function circle(cx, cy, r, color, alpha)
    local k = r * 0.5523
    return string.format("{\\an7\\pos(0,0)\\bord0\\shad0\\1c&H%s&\\1a&H%s&\\p1}m %d %d b %d %d %d %d %d %d b %d %d %d %d %d %d b %d %d %d %d %d %d b %d %d %d %d %d %d{\\p0}",
        hc(color), alpha, cx - r, cy, cx - r, cy - k, cx - k, cy - r, cx, cy - r, cx + k, cy - r, cx + r, cy - k, cx + r, cy,
        cx + r, cy + k, cx + k, cy + r, cx, cy + r, cx - k, cy + r, cx - r, cy + k, cx - r, cy)
end

local function spinner(parts, cx, cy, r, now)
    local n = 12
    local head = math.floor(now * 14) % n
    for i = 0, n - 1 do
        local a = (i / n) * 2 * math.pi - math.pi / 2
        local age = (head - i) % n -- 0 = forreste prik
        local alpha = math.min(255, 40 + age * 22)
        table.insert(parts, circle(cx + math.floor(r * math.cos(a)), cy + math.floor(r * math.sin(a)), math.max(4, math.floor(r / 6.5)),
            "FFFFFF", string.format("%02X", alpha)))
    end
end

local function status_draw()
    local now = mp.get_time()
    local parts = {}
    local busy = false
    if leaving then
        -- ending: no status
    elseif not started then
        busy = true
        local cx = video_w() / 2
        spinner(parts, cx, H / 2 - 40, 44, now)
        local title = meta.title
        if not title or title == "" then title = mp.get_property("media-title", "") end
        local ty = H / 2 + 60
        if title and title ~= "" and not title:match("^https?://") then
            for _, l in ipairs(wrap_px(title, video_w() - 240, 44, 2)) do
                table.insert(parts, text(cx, ty, 44, true, "FFFFFF", l, 8))
                ty = ty + 56
            end
        else
            ty = ty + 56
        end
        table.insert(parts, text(cx, ty + 4, 30, false, "B4B4B4", T("Loading…"), 8))
    elseif mp.get_property_native("paused-for-cache") then
        busy = true
        spinner(parts, W / 2, H / 2 - 20, 40, now)
        local pct = mp.get_property_number("cache-buffering-state")
        table.insert(parts, text(W / 2, H / 2 + 50, 30, false, "E6E6E6", pct and string.format(T("Buffering… %d %%"), pct) or T("Buffering…"), 8))
    elseif seek_t and now - seek_t > 0.3 then
        busy = true
        spinner(parts, W / 2, H / 2, 36, now)
    end
    if not busy and flash then
        local k = (now - flash_t) / 0.8
        if k >= 1 then
            flash = nil
        else
            busy = true
            local a = string.format("%02X", math.floor(math.max(0, k - 0.4) / 0.6 * 255))
            local ab = string.format("%02X", math.floor(0x60 + math.max(0, k - 0.4) / 0.6 * (255 - 0x60)))
            local cx, cy = W / 2, H / 2
            local r = math.floor(70 + 10 * ease(k / 0.4))
            table.insert(parts, circle(cx, cy, r, "000000", ab))
            if flash == "pause" then
                table.insert(parts, rect(cx - 26, cy - 32, 18, 64, "FFFFFF", a))
                table.insert(parts, rect(cx + 8, cy - 32, 18, 64, "FFFFFF", a))
            else
                table.insert(parts, string.format("{\\an7\\pos(0,0)\\bord0\\shad0\\1c&H%s&\\1a&H%s&\\p1}m %d %d l %d %d l %d %d{\\p0}",
                    hc("FFFFFF"), a, cx - 20, cy - 36, cx + 36, cy, cx - 20, cy + 36))
            end
        end
    end
    set_ov(ovs, table.concat(parts, "\n"))
    if busy then status_timer:resume() else status_timer:kill() end
end
status_timer = mp.add_periodic_timer(1 / 20, status_draw)
status_timer:kill()

mp.register_event("start-file", function() started = false; seek_t = nil; flash = nil; status_draw() end)
mp.register_event("playback-restart", function() started = true; seek_t = nil; status_draw() end)
mp.register_event("seek", function() if started then seek_t = mp.get_time(); status_draw() end end)
mp.register_event("end-file", function() set_ov(ovs, ""); status_timer:kill() end)
status_leave = function() leaving = true; status_draw() end
mp.observe_property("paused-for-cache", "bool", function() status_draw() end)
mp.observe_property("pause", "bool", function(_, p)
    if started and p ~= nil then flash, flash_t = p and "pause" or "play", mp.get_time() end
    status_draw()
end)
anim_timer:kill()
mp.observe_property("video-params/gamma", "string", function() update_hdr(); pq_cache = {}; render() end)
mp.register_event("file-loaded", function()
    -- YouTube cast: jump to the app's position, but only in videos with a length (live: start at "now")
    local cs = tonumber(mp.get_opt("notatv-start") or "") or 0
    local d = mp.get_property_number("duration", 0)
    if cs > 1 and d > 0 and cs < d - 5 then mp.commandv("seek", cs, "absolute") end
    load_meta()
    update_hdr()
    if meta.live then chan = load_chan() or chan end
    show(5)
end)
mp.observe_property("pause", "bool", function() render() end)
mp.register_event("seek", function() show(3) end)
timer = mp.add_periodic_timer(0.5, render)
load_meta()
