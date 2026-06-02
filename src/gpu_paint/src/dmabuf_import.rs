//! Vulkan dmabuf import for CEF accelerated-paint frames.
//!
//! Wraps a CEF dmabuf fd as a `wgpu::Texture` via the wgpu-hal Vulkan
//! backdoor (external memory + DRM format modifier). The frame is
//! re-imported on every paint: wgpu's resource tracking then inserts the
//! `UNDEFINED -> shader-read` transition (which makes the producer's
//! writes visible) and defers image/memory destruction until the GPU is
//! done with the submission, so no manual barriers or import caching are
//! needed. The import only aliases the fd — there is no pixel copy.

use std::ffi::CStr;
use std::os::fd::AsRawFd;

use ash::vk::Handle;
use ash::{ext, khr, vk};
use wgpu_hal::vulkan;

use crate::error::GpuPaintError;
use crate::types::{DmabufFormat, DmabufFrame};

/// `DRM_FORMAT_MOD_INVALID` — the producer did not negotiate an explicit
/// modifier; the buffer is in the driver's implicit layout.
const DRM_FORMAT_MOD_INVALID: u64 = 0x00ff_ffff_ffff_ffff;

/// Extensions wgpu-hal does not enable by default that we add via the
/// device-create callback when the physical device supports them. The
/// `external_memory_fd` / `external_memory_dma_buf` pair is already added
/// by wgpu-hal itself. `image_drm_format_modifier` is required to alias a
/// tiled dmabuf; `queue_family_foreign` is required for the ownership
/// barrier that acquires the producer-written image before sampling.
const EXTRA_EXTENSIONS: [&CStr; 2] = [
    ext::image_drm_format_modifier::NAME,
    ext::queue_family_foreign::NAME,
];

/// Extensions that must end up enabled for [`import`] to work.
const REQUIRED_EXTENSIONS: [&CStr; 4] = [
    khr::external_memory_fd::NAME,
    ext::external_memory_dma_buf::NAME,
    ext::image_drm_format_modifier::NAME,
    ext::queue_family_foreign::NAME,
];

fn ext_name(props: &vk::ExtensionProperties) -> &CStr {
    // SAFETY: the array is a NUL-terminated C string filled by the driver.
    unsafe { CStr::from_ptr(props.extension_name.as_ptr()) }
}

fn device_extensions(
    instance: &ash::Instance,
    phys: vk::PhysicalDevice,
) -> Vec<vk::ExtensionProperties> {
    unsafe { instance.enumerate_device_extension_properties(phys) }.unwrap_or_default()
}

/// The subset of [`EXTRA_EXTENSIONS`] the device actually supports —
/// pushed into the device-create extension list. Pushing an unsupported
/// extension makes `vkCreateDevice` hard-error, so this filter matters.
pub(crate) fn extra_device_extensions(
    instance: &ash::Instance,
    phys: vk::PhysicalDevice,
) -> Vec<&'static CStr> {
    let available = device_extensions(instance, phys);
    EXTRA_EXTENSIONS
        .into_iter()
        .filter(|want| available.iter().any(|p| ext_name(p) == *want))
        .collect()
}

/// Whether the device advertises every extension the import path needs.
fn required_extensions_present(instance: &ash::Instance, phys: vk::PhysicalDevice) -> bool {
    let available = device_extensions(instance, phys);
    REQUIRED_EXTENSIONS
        .iter()
        .all(|want| available.iter().any(|p| ext_name(p) == *want))
}

/// Post-build confirmation that the built device actually enabled every
/// required extension (the import-capable gate is only valid if so).
pub(crate) fn required_extensions_enabled(device: &wgpu::Device) -> bool {
    unsafe { device.as_hal::<vulkan::Api>() }
        .map(|d| {
            let enabled = d.enabled_device_extensions();
            REQUIRED_EXTENSIONS
                .iter()
                .all(|want| enabled.contains(want))
        })
        .unwrap_or(false)
}

fn vk_format(format: DmabufFormat) -> vk::Format {
    match format {
        DmabufFormat::Bgra8 => vk::Format::B8G8R8A8_UNORM,
        DmabufFormat::Rgba8 => vk::Format::R8G8B8A8_UNORM,
    }
}

fn wgpu_format(format: DmabufFormat) -> wgpu::TextureFormat {
    match format {
        DmabufFormat::Bgra8 => wgpu::TextureFormat::Bgra8Unorm,
        DmabufFormat::Rgba8 => wgpu::TextureFormat::Rgba8Unorm,
    }
}

/// Capability gate: required extensions present AND the device reports it
/// can import a dma_buf BGRA8 sampled image for at least one DRM modifier
/// it advertises. Read-only Vulkan queries; no allocation. Validates the
/// same-GPU case the overwhelmingly common deployment hits; a cross-GPU
/// modifier that still slips through fails per-frame at [`import`].
pub(crate) fn import_supported(instance: &ash::Instance, phys: vk::PhysicalDevice) -> bool {
    if !required_extensions_present(instance, phys) {
        return false;
    }
    unsafe { format_importable(instance, phys, vk::Format::B8G8R8A8_UNORM) }
}

unsafe fn format_importable(
    instance: &ash::Instance,
    phys: vk::PhysicalDevice,
    format: vk::Format,
) -> bool {
    // Two-call pattern: count, then fill the modifier list.
    let mut list = vk::DrmFormatModifierPropertiesListEXT::default();
    let mut props2 = vk::FormatProperties2::default().push_next(&mut list);
    unsafe { instance.get_physical_device_format_properties2(phys, format, &mut props2) };
    let count = list.drm_format_modifier_count as usize;
    if count == 0 {
        return false;
    }
    let mut mods = vec![vk::DrmFormatModifierPropertiesEXT::default(); count];
    let mut list =
        vk::DrmFormatModifierPropertiesListEXT::default().drm_format_modifier_properties(&mut mods);
    let mut props2 = vk::FormatProperties2::default().push_next(&mut list);
    unsafe { instance.get_physical_device_format_properties2(phys, format, &mut props2) };

    mods.iter().any(|m| {
        if !m
            .drm_format_modifier_tiling_features
            .contains(vk::FormatFeatureFlags::SAMPLED_IMAGE)
        {
            return false;
        }
        let mut external = vk::PhysicalDeviceExternalImageFormatInfo::default()
            .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
        let mut modifier = vk::PhysicalDeviceImageDrmFormatModifierInfoEXT::default()
            .drm_format_modifier(m.drm_format_modifier)
            .sharing_mode(vk::SharingMode::EXCLUSIVE);
        let info = vk::PhysicalDeviceImageFormatInfo2::default()
            .format(format)
            .ty(vk::ImageType::TYPE_2D)
            .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
            .usage(vk::ImageUsageFlags::SAMPLED)
            .push_next(&mut external)
            .push_next(&mut modifier);
        let mut external_props = vk::ExternalImageFormatProperties::default();
        let mut out = vk::ImageFormatProperties2::default().push_next(&mut external_props);
        let ok =
            unsafe { instance.get_physical_device_image_format_properties2(phys, &info, &mut out) }
                .is_ok();
        ok && external_props
            .external_memory_properties
            .external_memory_features
            .contains(vk::ExternalMemoryFeatureFlags::IMPORTABLE)
    })
}

/// Import one CEF dmabuf frame as a sampled `wgpu::Texture`. Returns the
/// texture (which owns the Vulkan image and imported memory, freed on drop
/// once the GPU is idle) and the raw `VkImage` handle, which the caller
/// passes to [`acquire_barrier`] before sampling.
///
/// # Safety
/// - `frame.planes[0].fd` must be a valid dmabuf fd describing a
///   `frame.width x frame.height` image in `frame.format`/`frame.modifier`.
pub(crate) unsafe fn import(
    device: &wgpu::Device,
    frame: &DmabufFrame,
) -> Result<(wgpu::Texture, u64), GpuPaintError> {
    if frame.planes.is_empty() {
        return Err(GpuPaintError::DmabufImport("no planes"));
    }

    let (hal_texture, image) = {
        let hal_device = unsafe { device.as_hal::<vulkan::Api>() }
            .ok_or(GpuPaintError::DmabufImport("not a Vulkan device"))?;
        unsafe { import_hal_texture(&hal_device, frame) }?
    };

    let desc = wgpu::TextureDescriptor {
        label: Some("jfn_gpu_paint dmabuf"),
        size: wgpu::Extent3d {
            width: frame.width,
            height: frame.height,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu_format(frame.format),
        usage: wgpu::TextureUsages::TEXTURE_BINDING,
        view_formats: &[],
    };
    let texture = unsafe { device.create_texture_from_hal::<vulkan::Api>(hal_texture, &desc) };
    Ok((texture, image.as_raw()))
}

/// Record a queue-family ownership acquire for an imported dmabuf image:
/// transfer ownership from the foreign producer to our graphics queue and
/// move it into the shader-read layout. Without this the GPU samples an
/// image it does not own and faults (device lost). Record this with the
/// raw HAL encoding API only; wgpu 29 forbids mixing raw and normal wgpu
/// commands in the same `CommandEncoder`.
pub(crate) fn acquire_barrier(
    device: &wgpu::Device,
    encoder: &mut wgpu::CommandEncoder,
    image: u64,
) {
    let cmd = unsafe { encoder.as_hal_mut::<vulkan::Api, _, _>(|e| e.map(|e| e.raw_handle())) };
    let Some(cmd) = cmd else { return };
    let Some(hal_device) = (unsafe { device.as_hal::<vulkan::Api>() }) else {
        return;
    };
    let ash_device = hal_device.raw_device();
    let barrier = vk::ImageMemoryBarrier::default()
        .src_access_mask(vk::AccessFlags::empty())
        .dst_access_mask(vk::AccessFlags::SHADER_READ)
        .old_layout(vk::ImageLayout::UNDEFINED)
        .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
        .src_queue_family_index(vk::QUEUE_FAMILY_FOREIGN_EXT)
        .dst_queue_family_index(0)
        .image(vk::Image::from_raw(image))
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::COLOR,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        });
    unsafe {
        ash_device.cmd_pipeline_barrier(
            cmd,
            vk::PipelineStageFlags::TOP_OF_PIPE,
            vk::PipelineStageFlags::FRAGMENT_SHADER,
            vk::DependencyFlags::empty(),
            &[],
            &[],
            &[barrier],
        );
    }
}

unsafe fn import_hal_texture(
    hal_device: &vulkan::Device,
    frame: &DmabufFrame,
) -> Result<(vulkan::Texture, vk::Image), GpuPaintError> {
    let plane = &frame.planes[0];
    let ash_device = hal_device.raw_device();
    let instance = hal_device.shared_instance().raw_instance();

    // --- create the image aliasing the dmabuf, no memory bound yet ---
    // CEF exports with the implicit modifier (`DRM_FORMAT_MOD_INVALID`) on
    // the X11/RADV path: there is no explicit tiling to describe, so import
    // with OPTIMAL tiling and let the (same) driver interpret its own
    // layout. Only an explicit modifier uses the DRM-modifier path (one
    // layout per memory plane; compressed modifiers add an auxiliary plane).
    let explicit = frame.modifier != DRM_FORMAT_MOD_INVALID;
    let plane_layouts: Vec<vk::SubresourceLayout> = frame
        .planes
        .iter()
        .map(|p| vk::SubresourceLayout {
            offset: p.offset,
            size: 0,
            row_pitch: p.stride as u64,
            array_pitch: 0,
            depth_pitch: 0,
        })
        .collect();
    let mut drm_info = vk::ImageDrmFormatModifierExplicitCreateInfoEXT::default()
        .drm_format_modifier(frame.modifier)
        .plane_layouts(&plane_layouts);
    let mut external_info = vk::ExternalMemoryImageCreateInfo::default()
        .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);
    let mut image_info = vk::ImageCreateInfo::default()
        .image_type(vk::ImageType::TYPE_2D)
        .format(vk_format(frame.format))
        .extent(vk::Extent3D {
            width: frame.width,
            height: frame.height,
            depth: 1,
        })
        .mip_levels(1)
        .array_layers(1)
        .samples(vk::SampleCountFlags::TYPE_1)
        .tiling(if explicit {
            vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT
        } else {
            vk::ImageTiling::OPTIMAL
        })
        .usage(vk::ImageUsageFlags::SAMPLED)
        .sharing_mode(vk::SharingMode::EXCLUSIVE)
        .initial_layout(vk::ImageLayout::UNDEFINED)
        .push_next(&mut external_info);
    if explicit {
        image_info = image_info.push_next(&mut drm_info);
    }
    let image = unsafe { ash_device.create_image(&image_info, None) }.map_err(|e| {
        tracing::warn!(
            "dmabuf vkCreateImage failed: modifier={:#018x} planes={} layouts=[{}] format={:?} size={}x{} result={:?}",
            frame.modifier,
            frame.planes.len(),
            frame
                .planes
                .iter()
                .map(|p| format!("off={} stride={}", p.offset, p.stride))
                .collect::<Vec<_>>()
                .join(", "),
            frame.format,
            frame.width,
            frame.height,
            e
        );
        GpuPaintError::DmabufImport("vkCreateImage")
    })?;

    let memory = match unsafe { import_memory(ash_device, instance, image, plane.fd.as_raw_fd()) } {
        Ok(m) => m,
        Err(e) => {
            unsafe { ash_device.destroy_image(image, None) };
            return Err(e);
        }
    };

    if unsafe { ash_device.bind_image_memory(image, memory, 0) }.is_err() {
        unsafe {
            ash_device.free_memory(memory, None);
            ash_device.destroy_image(image, None);
        }
        return Err(GpuPaintError::DmabufImport("vkBindImageMemory"));
    }

    let desc = wgpu_hal::TextureDescriptor {
        label: Some("jfn_gpu_paint dmabuf"),
        size: wgpu::Extent3d {
            width: frame.width,
            height: frame.height,
            depth_or_array_layers: 1,
        },
        mip_level_count: 1,
        sample_count: 1,
        dimension: wgpu::TextureDimension::D2,
        format: wgpu_format(frame.format),
        usage: wgpu::TextureUses::RESOURCE,
        memory_flags: wgpu_hal::MemoryFlags::empty(),
        view_formats: vec![],
    };
    // hal frees the image (drop_callback None) and the memory (Dedicated)
    // when the wgpu texture is dropped and the GPU is done with it.
    let texture = unsafe {
        hal_device.texture_from_raw(image, &desc, None, vulkan::TextureMemory::Dedicated(memory))
    };
    Ok((texture, image))
}

/// Allocate dedicated memory importing `fd` and matching `image`'s
/// requirements. Vulkan consumes a dup of `fd` on success; the caller's
/// fd is untouched.
unsafe fn import_memory(
    ash_device: &ash::Device,
    instance: &ash::Instance,
    image: vk::Image,
    fd: i32,
) -> Result<vk::DeviceMemory, GpuPaintError> {
    let mut dedicated_req = vk::MemoryDedicatedRequirements::default();
    let mut req2 = vk::MemoryRequirements2::default().push_next(&mut dedicated_req);
    let req_info = vk::ImageMemoryRequirementsInfo2::default().image(image);
    unsafe { ash_device.get_image_memory_requirements2(&req_info, &mut req2) };
    let size = req2.memory_requirements.size;

    let fd_loader = khr::external_memory_fd::Device::new(instance, ash_device);
    let mut fd_props = vk::MemoryFdPropertiesKHR::default();
    unsafe {
        fd_loader.get_memory_fd_properties(
            vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT,
            fd,
            &mut fd_props,
        )
    }
    .map_err(|_| GpuPaintError::DmabufImport("vkGetMemoryFdProperties"))?;

    let type_bits = req2.memory_requirements.memory_type_bits & fd_props.memory_type_bits;
    if type_bits == 0 {
        return Err(GpuPaintError::DmabufImport("no compatible memory type"));
    }
    let mem_type_index = type_bits.trailing_zeros();

    // Vulkan takes ownership of the fd on a successful allocate; hand it a dup.
    let import_fd = unsafe { libc::dup(fd) };
    if import_fd < 0 {
        return Err(GpuPaintError::DmabufImport("dup fd"));
    }
    let mut dedicated = vk::MemoryDedicatedAllocateInfo::default().image(image);
    let mut import_info = vk::ImportMemoryFdInfoKHR::default()
        .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT)
        .fd(import_fd);
    let alloc_info = vk::MemoryAllocateInfo::default()
        .allocation_size(size)
        .memory_type_index(mem_type_index)
        .push_next(&mut dedicated)
        .push_next(&mut import_info);
    match unsafe { ash_device.allocate_memory(&alloc_info, None) } {
        Ok(memory) => Ok(memory),
        Err(_) => {
            // fd not consumed on failure — close our dup.
            unsafe { libc::close(import_fd) };
            Err(GpuPaintError::DmabufImport("vkAllocateMemory"))
        }
    }
}
