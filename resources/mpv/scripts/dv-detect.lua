-- Detect Dolby Vision from mpv's track-list and set target-colorspace-hint
-- on Windows to prevent mpv from overriding the DV signal with HDR10

if mp.get_property("platform") ~= "windows" then
    return
end

mp.register_event("file-loaded", function()
    local tracks = mp.get_property_native("track-list", {})
    local is_dv = false
    for _, track in ipairs(tracks) do
        if track.type == "video" and track["dolby-vision-profile"] then
            is_dv = true
            break
        end
    end
    local hint = is_dv and "no" or "yes"
    mp.msg.info(string.format("[dv-detect] dolbyVision=%s → target-colorspace-hint=%s",
        tostring(is_dv), hint))
    mp.set_property("target-colorspace-hint", hint)
end)
