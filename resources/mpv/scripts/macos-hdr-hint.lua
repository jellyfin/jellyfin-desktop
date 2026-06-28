-- Enable HDR swapchain on macOS via target-colorspace-hint
--
-- Apple's display auto-detection cannot identify the dual-mode EDR display,
-- so libplacebo never requests an HDR swapchain without this hint.
-- Setting it unconditionally on macOS is correct: PQ/HLG/DV content all
-- benefit from the HDR swapchain, and SDR content is unaffected.

if mp.get_property("platform") ~= "darwin" then
    return
end

mp.register_event("file-loaded", function()
    mp.set_property("target-colorspace-hint", "yes")
    mp.msg.info("[macos-hdr-hint] target-colorspace-hint=yes")
end)
