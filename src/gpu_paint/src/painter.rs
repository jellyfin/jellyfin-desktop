//! Per-surface Vulkan compositor. Owns a swapchain, render pipeline,
//! and a persistent upload texture; presents BGRA pixels uploaded via
//! `queue.write_texture` (dirty-rect granularity).

use std::num::NonZeroU32;
use std::sync::Arc;

use raw_window_handle::{
    RawDisplayHandle, RawWindowHandle, WaylandDisplayHandle, WaylandWindowHandle, XcbDisplayHandle,
    XcbWindowHandle,
};

use crate::context::GpuContext;
use crate::error::GpuPaintError;
use crate::types::{PixelFrame, WindowTarget};

const SURFACE_FORMAT: wgpu::TextureFormat = wgpu::TextureFormat::Bgra8Unorm;

pub struct GpuPainter {
    ctx: Arc<GpuContext>,
    // 'static is a lie that wgpu accepts via `create_surface_unsafe`;
    // the caller guarantees the window outlives the painter (X11 owns
    // the xcb_window for the surface lifetime, Wayland likewise).
    surface: wgpu::Surface<'static>,
    config: wgpu::SurfaceConfiguration,
    pipeline: wgpu::RenderPipeline,
    bind_layout: wgpu::BindGroupLayout,
    sampler: wgpu::Sampler,
    // Persistent upload texture sized to the swapchain. Recreated on
    // resize. `None` until the first frame establishes a size.
    upload: Option<UploadTexture>,
    // Stored target size from the most recent `resize` call. Acts as
    // the gate: we only reconfigure (and present) once an incoming
    // frame matches it.
    pending_size: (u32, u32),
    visible: bool,
}

struct UploadTexture {
    tex: wgpu::Texture,
    bind_group: wgpu::BindGroup,
    w: u32,
    h: u32,
}

impl GpuPainter {
    pub fn new(
        ctx: Arc<GpuContext>,
        target: WindowTarget,
        size: (u32, u32),
    ) -> Result<Self, GpuPaintError> {
        if size.0 == 0 || size.1 == 0 {
            return Err(GpuPaintError::BadDimensions(size.0, size.1));
        }
        let max = ctx.device.limits().max_texture_dimension_2d;
        if size.0 > max || size.1 > max {
            return Err(GpuPaintError::BadDimensions(size.0, size.1));
        }

        let surface = unsafe { create_surface(&ctx.instance, target)? };

        if !ctx.adapter.is_surface_supported(&surface) {
            return Err(GpuPaintError::SurfaceUnsupported);
        }

        let caps = surface.get_capabilities(&ctx.adapter);
        let alpha_mode = pick_alpha_mode(&caps);

        let config = wgpu::SurfaceConfiguration {
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT,
            format: SURFACE_FORMAT,
            width: size.0,
            height: size.1,
            present_mode: wgpu::PresentMode::Fifo,
            desired_maximum_frame_latency: 2,
            alpha_mode,
            view_formats: vec![],
        };
        surface.configure(&ctx.device, &config);

        let shader = ctx
            .device
            .create_shader_module(wgpu::ShaderModuleDescriptor {
                label: Some("jfn_gpu_paint overlay"),
                source: wgpu::ShaderSource::Wgsl(include_str!("shaders/overlay.wgsl").into()),
            });

        let bind_layout = ctx
            .device
            .create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: Some("jfn_gpu_paint bgl"),
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: false },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::NonFiltering),
                        count: None,
                    },
                ],
            });

        let pipeline_layout = ctx
            .device
            .create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
                label: Some("jfn_gpu_paint pl"),
                bind_group_layouts: &[&bind_layout],
                push_constant_ranges: &[],
            });

        let pipeline = ctx
            .device
            .create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("jfn_gpu_paint pipeline"),
                layout: Some(&pipeline_layout),
                vertex: wgpu::VertexState {
                    module: &shader,
                    entry_point: "vs_main",
                    buffers: &[],
                    compilation_options: Default::default(),
                },
                fragment: Some(wgpu::FragmentState {
                    module: &shader,
                    entry_point: "fs_main",
                    compilation_options: Default::default(),
                    targets: &[Some(wgpu::ColorTargetState {
                        format: SURFACE_FORMAT,
                        blend: None,
                        write_mask: wgpu::ColorWrites::ALL,
                    })],
                }),
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    ..Default::default()
                },
                depth_stencil: None,
                multisample: wgpu::MultisampleState::default(),
                multiview: None,
                cache: None,
            });

        // Nearest, no anisotropy — 1:1 sampling, never stretch.
        let sampler = ctx.device.create_sampler(&wgpu::SamplerDescriptor {
            label: Some("jfn_gpu_paint sampler"),
            address_mode_u: wgpu::AddressMode::ClampToEdge,
            address_mode_v: wgpu::AddressMode::ClampToEdge,
            address_mode_w: wgpu::AddressMode::ClampToEdge,
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            mipmap_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });

        Ok(Self {
            ctx,
            surface,
            config,
            pipeline,
            bind_layout,
            sampler,
            upload: None,
            pending_size: size,
            visible: true,
        })
    }

    /// Store a new target size. Does not reconfigure the swapchain —
    /// next matching-size `push_pixels` does that. Mirrors the wayland
    /// `transitioning` gate: gaps acceptable during resize, stretching
    /// forbidden.
    pub fn resize(&mut self, size: (u32, u32)) {
        if size.0 == 0 || size.1 == 0 {
            return;
        }
        self.pending_size = size;
    }

    pub fn set_visible(&mut self, v: bool) {
        self.visible = v;
    }

    pub fn push_pixels(&mut self, frame: PixelFrame<'_>) -> Result<(), GpuPaintError> {
        if frame.width == 0 || frame.height == 0 {
            return Err(GpuPaintError::BadDimensions(frame.width, frame.height));
        }
        let max = self.ctx.device.limits().max_texture_dimension_2d;
        if frame.width > max || frame.height > max {
            return Err(GpuPaintError::BadDimensions(frame.width, frame.height));
        }
        if !self.visible {
            return Ok(());
        }

        // Reconfigure swapchain whenever the incoming frame's size
        // differs from the current config. CEF authors the frame at
        // its own pace after a resize; presenting at the producer's
        // size keeps content 1:1 (no stretching) and avoids stalling
        // on a `pending_size` that never matches.
        if (self.config.width, self.config.height) != (frame.width, frame.height) {
            self.config.width = frame.width;
            self.config.height = frame.height;
            self.surface.configure(&self.ctx.device, &self.config);
            self.upload = None;
            self.pending_size = (frame.width, frame.height);
        }

        self.ensure_upload(frame.width, frame.height);
        let upload = self.upload.as_ref().unwrap();

        if frame.dirty.is_empty() {
            write_rect(
                &self.ctx.queue,
                upload,
                &frame,
                0,
                0,
                frame.width as i32,
                frame.height as i32,
            );
        } else {
            for r in frame.dirty {
                let (x, y, w, h) =
                    clip_rect(r.x, r.y, r.w, r.h, frame.width as i32, frame.height as i32);
                if w <= 0 || h <= 0 {
                    continue;
                }
                write_rect(&self.ctx.queue, upload, &frame, x, y, w, h);
            }
        }

        self.draw_and_present()
    }

    pub fn shutdown(self) {
        // Drop order matters: surface before device queue; wgpu handles
        // it via field order. Explicit method is here so callers
        // signal intent and we can extend later if needed.
        drop(self);
    }

    // ----- internals -----

    fn ensure_upload(&mut self, w: u32, h: u32) {
        let needs_new = self.upload.as_ref().is_none_or(|u| u.w != w || u.h != h);
        if needs_new {
            let tex = self.ctx.device.create_texture(&wgpu::TextureDescriptor {
                label: Some("jfn_gpu_paint upload"),
                size: wgpu::Extent3d {
                    width: w,
                    height: h,
                    depth_or_array_layers: 1,
                },
                mip_level_count: 1,
                sample_count: 1,
                dimension: wgpu::TextureDimension::D2,
                format: SURFACE_FORMAT,
                usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
                view_formats: &[],
            });
            let view = tex.create_view(&wgpu::TextureViewDescriptor::default());
            let bind_group = self
                .ctx
                .device
                .create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("jfn_gpu_paint bg"),
                    layout: &self.bind_layout,
                    entries: &[
                        wgpu::BindGroupEntry {
                            binding: 0,
                            resource: wgpu::BindingResource::TextureView(&view),
                        },
                        wgpu::BindGroupEntry {
                            binding: 1,
                            resource: wgpu::BindingResource::Sampler(&self.sampler),
                        },
                    ],
                });
            self.upload = Some(UploadTexture {
                tex,
                bind_group,
                w,
                h,
            });
        }
    }

    fn draw_and_present(&mut self) -> Result<(), GpuPaintError> {
        let frame = match self.surface.get_current_texture() {
            Ok(f) => f,
            Err(wgpu::SurfaceError::Lost | wgpu::SurfaceError::Outdated) => {
                self.surface.configure(&self.ctx.device, &self.config);
                self.surface
                    .get_current_texture()
                    .map_err(GpuPaintError::Acquire)?
            }
            Err(e) => return Err(GpuPaintError::Acquire(e)),
        };
        let view = frame
            .texture
            .create_view(&wgpu::TextureViewDescriptor::default());
        let mut encoder = self
            .ctx
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor {
                label: Some("jfn_gpu_paint enc"),
            });
        {
            let upload = self.upload.as_ref().expect("upload texture initialized");
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("jfn_gpu_paint pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::TRANSPARENT),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                occlusion_query_set: None,
                timestamp_writes: None,
            });
            pass.set_pipeline(&self.pipeline);
            pass.set_bind_group(0, &upload.bind_group, &[]);
            pass.draw(0..3, 0..1);
        }
        self.ctx.queue.submit(std::iter::once(encoder.finish()));
        frame.present();
        Ok(())
    }
}

fn clip_rect(x: i32, y: i32, w: i32, h: i32, fw: i32, fh: i32) -> (i32, i32, i32, i32) {
    let mut nx = x.max(0);
    let mut ny = y.max(0);
    let mut nw = w + x.min(0);
    let mut nh = h + y.min(0);
    if nx + nw > fw {
        nw = fw - nx;
    }
    if ny + nh > fh {
        nh = fh - ny;
    }
    if nw < 0 {
        nw = 0;
    }
    if nh < 0 {
        nh = 0;
    }
    // Shadow check: starting offset still in-bounds.
    if nx >= fw {
        nx = fw - 1;
        nw = 0;
    }
    if ny >= fh {
        ny = fh - 1;
        nh = 0;
    }
    (nx, ny, nw, nh)
}

fn write_rect(
    queue: &wgpu::Queue,
    upload: &UploadTexture,
    frame: &PixelFrame<'_>,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
) {
    let stride = frame.stride as usize;
    let start = (y as usize) * stride + (x as usize) * 4;
    let end = start + ((h - 1) as usize) * stride + (w as usize) * 4;
    let slice = &frame.bgra[start..end];
    queue.write_texture(
        wgpu::ImageCopyTexture {
            texture: &upload.tex,
            mip_level: 0,
            origin: wgpu::Origin3d {
                x: x as u32,
                y: y as u32,
                z: 0,
            },
            aspect: wgpu::TextureAspect::All,
        },
        slice,
        wgpu::ImageDataLayout {
            offset: 0,
            bytes_per_row: Some(frame.stride),
            rows_per_image: NonZeroU32::new(h as u32).map(|n| n.get()),
        },
        wgpu::Extent3d {
            width: w as u32,
            height: h as u32,
            depth_or_array_layers: 1,
        },
    );
}

fn pick_alpha_mode(caps: &wgpu::SurfaceCapabilities) -> wgpu::CompositeAlphaMode {
    use wgpu::CompositeAlphaMode::*;
    [PreMultiplied, PostMultiplied, Inherit, Opaque, Auto]
        .into_iter()
        .find(|m| caps.alpha_modes.contains(m))
        .unwrap_or(Auto)
}

unsafe fn create_surface(
    instance: &wgpu::Instance,
    target: WindowTarget,
) -> Result<wgpu::Surface<'static>, GpuPaintError> {
    let (display, window) = match target {
        WindowTarget::Xcb {
            connection,
            window,
            screen,
            visual,
        } => {
            let display = XcbDisplayHandle::new(Some(connection.cast()), screen);
            let mut wh = XcbWindowHandle::new(
                NonZeroU32::new(window).ok_or(GpuPaintError::SurfaceUnsupported)?,
            );
            wh.visual_id = NonZeroU32::new(visual);
            (RawDisplayHandle::Xcb(display), RawWindowHandle::Xcb(wh))
        }
        WindowTarget::Wayland { display, surface } => {
            let dh = WaylandDisplayHandle::new(display);
            let wh = WaylandWindowHandle::new(surface);
            (RawDisplayHandle::Wayland(dh), RawWindowHandle::Wayland(wh))
        }
    };
    let surface = unsafe {
        instance.create_surface_unsafe(wgpu::SurfaceTargetUnsafe::RawHandle {
            raw_display_handle: display,
            raw_window_handle: window,
        })?
    };
    Ok(surface)
}
