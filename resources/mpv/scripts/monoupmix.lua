local mp = require 'mp'
local msg = require 'mp.msg'

local TAG = "monoupmix"
local applied = false

local function selected_source_channels()
    local n = mp.get_property_number("track-list/count", 0)
    for i = 0, n - 1 do
        local t = string.format("track-list/%d/", i)
        if mp.get_property(t .. "type") == "audio"
            and mp.get_property_bool(t .. "selected", false) then
            return mp.get_property_number(t .. "demux-channel-count")
        end
    end
    return nil
end

local function update()
    local mono = selected_source_channels() == 1
    if mono and not applied then
        mp.commandv("no-osd", "af", "append",
            "@" .. TAG .. ":lavfi=[pan=stereo|c0=c0|c1=c0]")
        applied = true
        msg.info("mono source detected -> duplicating to stereo")
    elseif not mono and applied then
        mp.commandv("no-osd", "af", "remove", "@" .. TAG)
        applied = false
    end
end

mp.register_event("file-loaded", update)
mp.observe_property("current-tracks/audio/id", "native", update)
