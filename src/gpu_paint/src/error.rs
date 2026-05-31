use thiserror::Error;

#[derive(Debug, Error)]
pub enum GpuPaintError {
    #[error("no Vulkan adapter available")]
    NoAdapter,
    #[error("wgpu device request failed: {0}")]
    DeviceRequest(#[from] wgpu::RequestDeviceError),
    #[error("surface creation failed: {0}")]
    SurfaceCreate(#[from] wgpu::CreateSurfaceError),
    #[error("adapter does not support requested surface")]
    SurfaceUnsupported,
    #[error("swapchain acquire failed: {0}")]
    Acquire(&'static str),
    #[error("invalid frame dimensions: {0}x{1}")]
    BadDimensions(u32, u32),
}
