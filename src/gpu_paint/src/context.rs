//! Vulkan-only wgpu device. Singleton, shared across surfaces.

use std::sync::Arc;

use crate::error::GpuPaintError;

#[derive(Copy, Clone, Debug)]
pub struct Capabilities {
    pub gpu_available: bool,
    /// Reserved for v1 (wgpu-hal Vulkan backdoor + dmabuf import).
    /// v0 always reports false.
    pub dmabuf_import: bool,
}

impl Capabilities {
    pub const NONE: Self = Self {
        gpu_available: false,
        dmabuf_import: false,
    };
}

pub struct GpuContext {
    pub(crate) instance: wgpu::Instance,
    pub(crate) adapter: wgpu::Adapter,
    pub(crate) device: wgpu::Device,
    pub(crate) queue: wgpu::Queue,
    caps: Capabilities,
}

impl GpuContext {
    /// Cheap probe: enumerate Vulkan adapters and pick one. Does not
    /// create a `Device` and does not need a surface.
    pub fn probe() -> Capabilities {
        let instance = build_instance();
        match pick_adapter(&instance) {
            Some(adapter) => {
                let info = adapter.get_info();
                tracing::info!(
                    name = %info.name,
                    backend = ?info.backend,
                    device_type = ?info.device_type,
                    "gpu_paint: probe found Vulkan adapter"
                );
                Capabilities {
                    gpu_available: true,
                    dmabuf_import: false,
                }
            }
            None => {
                tracing::info!("gpu_paint: probe found no Vulkan adapter");
                Capabilities::NONE
            }
        }
    }

    /// Build the full context. Blocks on device creation (wgpu's async
    /// future resolves synchronously on the native Vulkan backend; we
    /// use `pollster` rather than dragging in tokio).
    pub fn new() -> Result<Arc<Self>, GpuPaintError> {
        let instance = build_instance();
        let adapter = pick_adapter(&instance).ok_or(GpuPaintError::NoAdapter)?;
        let info = adapter.get_info();

        let (device, queue) = pollster::block_on(adapter.request_device(
            &wgpu::DeviceDescriptor {
                label: Some("jfn_gpu_paint device"),
                required_features: wgpu::Features::empty(),
                // Adapter limits — the swapchain may be larger than the
                // downlevel 2048×2048 cap on modern displays.
                required_limits: adapter.limits(),
                memory_hints: wgpu::MemoryHints::Performance,
            },
            None,
        ))?;

        tracing::info!(
            adapter = %info.name,
            backend = ?info.backend,
            "gpu_paint: device created"
        );

        Ok(Arc::new(Self {
            instance,
            adapter,
            device,
            queue,
            caps: Capabilities {
                gpu_available: true,
                dmabuf_import: false,
            },
        }))
    }

    pub fn capabilities(&self) -> Capabilities {
        self.caps
    }
}

fn build_instance() -> wgpu::Instance {
    wgpu::Instance::new(wgpu::InstanceDescriptor {
        backends: wgpu::Backends::VULKAN,
        flags: wgpu::InstanceFlags::empty(),
        dx12_shader_compiler: wgpu::Dx12Compiler::default(),
        gles_minor_version: wgpu::Gles3MinorVersion::default(),
    })
}

fn pick_adapter(instance: &wgpu::Instance) -> Option<wgpu::Adapter> {
    let adapters: Vec<_> = instance
        .enumerate_adapters(wgpu::Backends::VULKAN)
        .into_iter()
        .filter(|a| {
            !matches!(
                a.get_info().device_type,
                wgpu::DeviceType::Cpu | wgpu::DeviceType::Other
            )
        })
        .collect();

    adapters
        .into_iter()
        .max_by_key(|a| match a.get_info().device_type {
            wgpu::DeviceType::DiscreteGpu => 3,
            wgpu::DeviceType::IntegratedGpu => 2,
            wgpu::DeviceType::VirtualGpu => 1,
            _ => 0,
        })
}
