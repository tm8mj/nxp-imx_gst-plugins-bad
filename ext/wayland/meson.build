wl_sources = [
    'gstwaylandsink.c'
]

ionallocator_flags = []

if cc.has_header('linux/ion.h') and cc.has_header('gst/allocators/gstionmemory.h', dependencies: gstallocators_dep)
    ionallocator_flags += ['-DHAVE_ION_ALLOCATOR']
endif

dmabufheapsallocator_flags = []

if cc.has_header('linux/dma-heap.h') and cc.has_header('gst/allocators/gstdmabufheaps.h', dependencies: gstallocators_dep)
    dmabufheapsallocator_flags += ['-DHAVE_DMABUFHEAPS_ALLOCATOR']
endif

if use_wayland

    gstwaylandsink = library('gstwaylandsink',
        wl_sources + protocols_files,
        c_args : gst_plugins_bad_args + ionallocator_flags + dmabufheapsallocator_flags + ['-DGST_USE_UNSTABLE_API'],
        include_directories : [configinc],
        dependencies : [gst_dep, gstvideo_dep, gstwayland_dep,
                        wl_client_dep, wl_protocol_dep, libdrm_dep],
        install : true,
        install_dir : plugins_install_dir,
    )
    plugins += [gstwaylandsink]
endif
