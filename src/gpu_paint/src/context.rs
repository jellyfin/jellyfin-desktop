//! Vulkan-only wgpu device. Singleton, shared across surfaces.

use std::sync::Arc;

use wgpu_hal::vulkan;

use crate::dmabuf_import;
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
                let dmabuf_import = probe_dmabuf_import(&adapter);
                tracing::info!(
                    name = %info.name,
                    backend = ?info.backend,
                    device_type = ?info.device_type,
                    dmabuf_import,
                    "gpu_paint: probe found Vulkan adapter"
                );
                Capabilities {
                    gpu_available: true,
                    dmabuf_import,
                }
            }
            None => {
                tracing::info!("gpu_paint: probe found no Vulkan adapter");
                Capabilities::NONE
            }
        }
    }

    pub fn new() -> Result<Arc<Self>, GpuPaintError> {
        let instance = build_instance();
        let adapter = pick_adapter(&instance).ok_or(GpuPaintError::NoAdapter)?;
        let info = adapter.get_info();
        let limits = adapter.limits();

        let (want_dmabuf, extra_exts) = unsafe {
            match adapter.as_hal::<vulkan::Api>() {
                Some(hal) => {
                    let ash_instance = hal.shared_instance().raw_instance();
                    let phys = hal.raw_physical_device();
                    (
                        dmabuf_import::import_supported(ash_instance, phys),
                        dmabuf_import::extra_device_extensions(ash_instance, phys),
                    )
                }
                None => (false, Vec::new()),
            }
        };

        let open_device = unsafe {
            let hal = adapter
                .as_hal::<vulkan::Api>()
                .ok_or(GpuPaintError::NoAdapter)?;
            hal.open_with_callback(
                wgpu::Features::empty(),
                &limits,
                &wgpu::MemoryHints::Performance,
                Some(Box::new(move |args: vulkan::CreateDeviceCallbackArgs| {
                    for ext in &extra_exts {
                        if !args.extensions.contains(ext) {
                            args.extensions.push(*ext);
                        }
                    }
                })),
            )
            .map_err(|_| GpuPaintError::NoAdapter)?
        };

        let (device, queue) = unsafe {
            adapter.create_device_from_hal::<vulkan::Api>(
                open_device,
                &wgpu::DeviceDescriptor {
                    label: Some("jfn_gpu_paint device"),
                    required_features: wgpu::Features::empty(),
                    // Adapter limits — the swapchain may be larger than the
                    // downlevel 2048×2048 cap on modern displays.
                    required_limits: limits,
                    experimental_features: wgpu::ExperimentalFeatures::default(),
                    memory_hints: wgpu::MemoryHints::Performance,
                    trace: wgpu::Trace::Off,
                },
            )?
        };

        device.set_device_lost_callback(|reason, msg| {
            tracing::error!("gpu_paint: DEVICE LOST: {reason:?}: {msg}");
        });
        device.on_uncaptured_error(std::sync::Arc::new(|e: wgpu::Error| {
            tracing::error!("gpu_paint: wgpu error: {e}");
        }));

        let dmabuf_import = want_dmabuf && dmabuf_import::required_extensions_enabled(&device);

        tracing::info!(
            adapter = %info.name,
            backend = ?info.backend,
            dmabuf_import,
            "gpu_paint: device created"
        );

        Ok(Arc::new(Self {
            instance,
            adapter,
            device,
            queue,
            caps: Capabilities {
                gpu_available: true,
                dmabuf_import,
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
        ..wgpu::InstanceDescriptor::new_without_display_handle()
    })
}

fn probe_dmabuf_import(adapter: &wgpu::Adapter) -> bool {
    unsafe {
        adapter
            .as_hal::<vulkan::Api>()
            .map(|hal| {
                dmabuf_import::import_supported(
                    hal.shared_instance().raw_instance(),
                    hal.raw_physical_device(),
                )
            })
            .unwrap_or(false)
    }
}

fn pick_adapter(instance: &wgpu::Instance) -> Option<wgpu::Adapter> {
    let adapters: Vec<_> = pollster::block_on(instance.enumerate_adapters(wgpu::Backends::VULKAN))
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
