#include "pack.h"
#include "util_rt.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <zlib.h>


void prne_init_bin_archive (prne_bin_archive_t *a) {
    a->data_size = 0;
    a->data = NULL;
    a->nb_binaries = 0;
    a->arch_arr = NULL;
    a->offset_arr = NULL;
    a->size_arr = NULL;    
}

void prne_init_unpack_bin_archive_result (prne_unpack_bin_archive_result_t *r) {
    r->data_size = 0;
    r->data = NULL;
    r->result = PRNE_UNPACK_BIN_ARCHIVE_OK;
    r->err = 0;
}

prne_unpack_bin_archive_result_t prne_unpack_bin_archive (const int fd) {
    static const size_t fd_buf_size = 77, bio_buf_size = 58, z_buf_size = 4096;
    
    prne_unpack_bin_archive_result_t ret;
    BIO *b64_bio = NULL, *mem_bio = NULL;
    uint8_t *mem = NULL, *fd_buf = NULL, *bio_buf = NULL, *z_buf = NULL;
    int fd_read_size, fd_data_size, bio_write_size, bio_read_size;
    int z_func_ret;
    z_stream stream;
    size_t z_out_size;
    void *ny_buf;
    bool stream_end;

    prne_init_unpack_bin_archive_result(&ret);
    memset(&stream, 0, sizeof(z_stream));

    mem = (uint8_t*)prne_malloc(1, fd_buf_size + bio_buf_size + z_buf_size);
    if (mem == NULL) {
        ret.result = PRNE_UNPACK_BIN_ARCHIVE_MEM_ERR;
        ret.err = errno;
        goto END;
    }
    fd_buf = mem;
    bio_buf = mem + fd_buf_size;
    z_buf = mem + fd_buf_size + bio_buf_size;

    z_func_ret = inflateInit(&stream);
    if (z_func_ret != Z_OK) {
        ret.result = PRNE_UNPACK_BIN_ARCHIVE_Z_ERR;
        ret.err = z_func_ret;
        goto END;
    }

    if ((mem_bio = BIO_new(BIO_s_mem())) == NULL || (b64_bio = BIO_new(BIO_f_base64())) == NULL) {
        ret.result = PRNE_UNPACK_BIN_ARCHIVE_OPENSSL_ERR;
        ret.err = ERR_get_error();
        goto END;
    }
    BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64_bio, mem_bio);

    stream_end = false;
    do {
        fd_read_size = read(fd, fd_buf, fd_buf_size);
        if (fd_read_size < 0) {
            ret.result = PRNE_UNPACK_BIN_ARCHIVE_ERRNO;
            ret.err = errno;
            goto END;
        }
        if (fd_read_size == 0) {
            break;
        }

        // remove white spaces
        fd_data_size = prne_str_shift_spaces((char*)fd_buf, (size_t)fd_read_size);

        if (fd_data_size > 0) {
            BIO_reset(mem_bio);
            bio_write_size = BIO_write(mem_bio, fd_buf, fd_data_size);
            if (bio_write_size != fd_data_size) {
                ret.result = PRNE_UNPACK_BIN_ARCHIVE_MEM_ERR;
                goto END;
            }

            bio_read_size = BIO_read(b64_bio, bio_buf, (int)bio_buf_size);
            if (bio_read_size < 0) {
                ret.result = PRNE_UNPACK_BIN_ARCHIVE_OPENSSL_ERR;
                ret.err = ERR_get_error();
                goto END;
            }

            if (bio_read_size > 0) {
                stream.avail_in = bio_read_size;
                stream.next_in = bio_buf;
                do {
                    stream.avail_out = z_buf_size;
                    stream.next_out = z_buf;
                    z_func_ret = inflate(&stream, Z_NO_FLUSH);
                    switch (z_func_ret) {
                    case Z_STREAM_END:
                        stream_end = true;
                        break;
                    case Z_OK:
                    case Z_BUF_ERROR:
                        break;
                    default:
                        ret.result = PRNE_UNPACK_BIN_ARCHIVE_Z_ERR;
                        ret.err = z_func_ret;
                        goto END;
                    } 

                    z_out_size = z_buf_size - stream.avail_out;
                    if (z_out_size > 0) {
                        ny_buf = prne_realloc(ret.data, 1, ret.data_size + z_out_size);
                        if (ny_buf == NULL) {
                            ret.result = PRNE_UNPACK_BIN_ARCHIVE_MEM_ERR;
                            ret.err = errno;
                            break;
                        }
                        ret.data = (uint8_t*)ny_buf;

                        memcpy(ret.data + ret.data_size, z_buf, z_out_size);
                        ret.data_size += z_out_size;
                    }
                } while (stream.avail_out == 0);            
            }
        }
    } while (!stream_end);

    if (ret.data_size == 0) {
        ret.result = PRNE_UNPACK_BIN_ARCHIVE_FMT_ERR;
    }

END:
    prne_free(mem);
    if (ret.result != PRNE_UNPACK_BIN_ARCHIVE_OK) {
        prne_free(ret.data);
        ret.data = NULL;
        ret.data_size = 0;
    }
    inflateEnd(&stream);
    BIO_free(b64_bio);
    BIO_free(mem_bio);

    return ret;
}

prne_index_bin_archive_result_code_t prne_index_bin_archive (prne_unpack_bin_archive_result_t *in, prne_bin_archive_t *out) {
    prne_index_bin_archive_result_code_t ret = PRNE_INDEX_BIN_ARCHIVE_OK;
    size_t buf_pos = 0, arr_cnt = 0, offset_arr[NB_PRNE_ARCH], size_arr[NB_PRNE_ARCH];
    prne_arch_t arch;
    uint32_t bin_size;
    prne_arch_t arch_arr[NB_PRNE_ARCH];
    prne_bin_archive_t archive;
    
    memset(arch_arr, 0, sizeof(prne_arch_t) * NB_PRNE_ARCH);
    memset(offset_arr, 0, sizeof(size_t) * NB_PRNE_ARCH);
    memset(size_arr, 0, sizeof(size_t) * NB_PRNE_ARCH);
    prne_init_bin_archive(&archive);

    do {
        if (buf_pos + 4 >= in->data_size || arr_cnt >= NB_PRNE_ARCH) {
            ret = PRNE_INDEX_BIN_ARCHIVE_FMT_ERR;
            goto END;
        }

        arch = (prne_arch_t)in->data[buf_pos];
        bin_size =
            ((uint32_t)in->data[buf_pos + 1] << 16) |
            ((uint32_t)in->data[buf_pos + 2] << 8) |
            (uint32_t)in->data[buf_pos + 3];
        if (prne_arch2str(arch) == NULL || bin_size == 0 || buf_pos + 4 + bin_size > in->data_size) {
            ret = PRNE_INDEX_BIN_ARCHIVE_FMT_ERR;
            goto END;
        }

        arch_arr[arr_cnt] = arch;
        offset_arr[arr_cnt] = 4 + buf_pos;
        size_arr[arr_cnt] = bin_size;
        arr_cnt += 1;
        
        buf_pos += 4 + bin_size;
    } while (buf_pos < in->data_size);

    archive.arch_arr = (prne_arch_t*)prne_malloc(sizeof(prne_arch_t), arr_cnt);
    archive.offset_arr = (size_t*)prne_malloc(sizeof(size_t), arr_cnt);
    archive.size_arr = (size_t*)prne_malloc(sizeof(size_t), arr_cnt);
    if (archive.arch_arr == NULL || archive.offset_arr == NULL || archive.size_arr == NULL) {
        ret = PRNE_INDEX_BIN_ARCHIVE_MEM_ERR;
        goto END;
    }

    archive.data_size = in->data_size;
    archive.data = in->data;
    archive.nb_binaries = arr_cnt;
    memcpy(archive.arch_arr, arch_arr, arr_cnt * sizeof(prne_arch_t));
    memcpy(archive.offset_arr, offset_arr, arr_cnt * sizeof(size_t));
    memcpy(archive.size_arr, size_arr, arr_cnt * sizeof(size_t));

    in->data = NULL;
    in->data_size = 0;
    *out = archive;

END:
    if (ret != PRNE_INDEX_BIN_ARCHIVE_OK) {
        prne_free_bin_archive(&archive);
    }

    return ret;
}

void prne_free_unpack_bin_archive_result (prne_unpack_bin_archive_result_t *r) {
    prne_free(r->data);
    r->data = NULL;
    r->data_size = 0;
    r->result = PRNE_INDEX_BIN_ARCHIVE_OK;
    r->err = 0;
}

void prne_free_bin_archive (prne_bin_archive_t *a) {
    prne_free(a->data);
    prne_free(a->arch_arr);
    prne_free(a->offset_arr);
    prne_free(a->size_arr);
    a->nb_binaries = 0;
    a->data = NULL;
    a->data_size = 0;
    a->arch_arr = NULL;
    a->offset_arr = NULL;
    a->size_arr = NULL;
}
