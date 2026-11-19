# The Android container's GPU (after 65-anco.sh). The guest loads its GPU driver and gralloc from the host's
# /vendor/anco_spec/vendor (Kirin's on the donor); ours, from common/apps/build-anco-adreno.sh, is Adreno's GLES and
# Vulkan with QTI gralloc from Nothing's vendor, in the asteroids partition at /asteroids/anco with the guest's labels.
# With it: the GPU and the dma-buf heaps QTI gralloc allocates from, and the guest's libgui patched to keep its surface
# delegator on the protocol our broker speaks.
OV=$R/system/variant/phone/base/etc/anco/anco_override_hostconfig.json
if [ -d $A/anco/vendor ] && [ -f $OV ] && [ -d $R/vendor/anco_spec/vendor ]; then
    mount --bind $A/anco/vendor $R/vendor/anco_spec/vendor || log "anco: Adreno anco_spec not bound"
    grep -q '"/dev/kgsl-3d0"' $OV || sed -i 's|"Devices": \[|"Devices": [\
    {"CgroupPermissions": "rwm", "PathInContainer": "/dev/kgsl-3d0", "PathOnHost": "/dev/kgsl-3d0", "SourceMustExist": false},\
    {"CgroupPermissions": "rwm", "PathInContainer": "/dev/dma_heap/system", "PathOnHost": "/dev/dma_heap/system", "SourceMustExist": false},\
    {"CgroupPermissions": "rwm", "PathInContainer": "/dev/dma_heap/qcom,system", "PathOnHost": "/dev/dma_heap/qcom,system", "SourceMustExist": false},\
    {"CgroupPermissions": "rwm", "PathInContainer": "/dev/dma_heap/qcom,display", "PathOnHost": "/dev/dma_heap/qcom,display", "SourceMustExist": false},|' $OV
    grep -q '"/asteroids/anco/system/lib64/libgui.so"' $OV || sed -i 's|"Mounts": \[|"Mounts": [\
    {"Type": "bind", "Source": "/asteroids/anco/system/lib64/libgui.so", "Target": "/system/lib64/libgui.so", "SourceMustExist": true},|' $OV
    log "anco: Adreno anco_spec, GPU nodes and patched libgui"
fi
