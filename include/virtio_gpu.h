#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

/*
 * Public hooks of the virtio-gpu frontend (phase 9). The fb core
 * stays backend-agnostic; only the test/present path needs the
 * explicit whole-frame push this backend implements.
 */

int  fb_virtio_gpu_present(void);
void fb_virtio_gpu_backend_register(void);

#endif /* VIRTIO_GPU_H */