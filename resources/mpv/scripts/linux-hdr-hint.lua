-- linux-hdr-hint.lua
-- Sets target-colorspace-hint=yes on every file load so the Wayland compositor
-- can allocate an HDR surface. Requires GNOME 46+ with HDR experimental feature:
--   gsettings set org.gnome.mutter experimental-features "['hdr']"

if mp.get_property("platform") ~= "linux" then
    return
end

mp.register_event("file-loaded", function()
    mp.set_property("target-colorspace-hint", "yes")
end)
