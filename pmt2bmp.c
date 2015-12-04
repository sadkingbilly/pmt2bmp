#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/*
    PMT is a deprecated graphics format used as output by some old handheld scanners.
    This utility converts PMT files (at least some of them) to a more modern BMP format.

    The PMT file contains 6 RLE-encoded groups (see pmt_decode_group()) which decode
    to 62160 bytes each. Each of the decoded byte groups contains information about
    148 pixel rows, each row encoded as 420 bytes with 4 bits per pixel, so maximum
    encoded picture size is 840 (width) by 888 (height) pixels. Each 420-byte row
    consists of 4 consecutive 105-byte blocks, with block encoding row information
    (840 bits, 1 bit per pixel) for one of the VGA planes. In order to reconstruct the
    4-bit color value for each pixel, the pixel's bit value from each of the blocks/planes
    needs to be combined into a 4-bit integer (see bmp_pixel()).

    The pixel information is followed by a 64-byte footer. The last 48 bytes of the footer
    contain the color table represented as 16 3-byte entries of 6-bit R/G/B color values.
    The color of each pixel is determined by a lookup of the decoded pixel value into this
    table.
*/


/* Single pixel row size for one VGA plane, 1 bit per pixel. */
#define PMT_DECODED_ROW_PLANE_BYTES 105

#define PMT_PIXELS_PER_ROW (PMT_DECODED_ROW_PLANE_BYTES * 8)

/* Number of VGA planes, same as number of bits per pixel. */
#define PMT_PLANES 4

/* Complete pixel row size, for all planes. */
#define PMT_DECODED_ROW_BYTES (PMT_DECODED_ROW_PLANE_BYTES * PMT_PLANES)

/* BMP pixel row size, 4 bits per pixel. */
#define BMP_ROW_BYTES PMT_DECODED_ROW_BYTES 

/* Pixel rows per group. */
#define PMT_ROWS_PER_GROUP 148

/* Complete group size. */
#define PMT_DECODED_GROUP_BYTES (PMT_DECODED_ROW_BYTES * PMT_ROWS_PER_GROUP)

#define BMP_GROUP_BYTES (BMP_ROW_BYTES * PMT_ROWS_PER_GROUP)

/* Number of groups. */
#define PMT_GROUPS 6

#define BMP_PIXEL_ARRAY_SIZE (BMP_GROUP_BYTES * PMT_GROUPS)

#define PMT_ROWS (PMT_ROWS_PER_GROUP * PMT_GROUPS)


/* Types */

#pragma pack(push, 1)

typedef uint8_t pmt_row_t[PMT_PLANES][PMT_DECODED_ROW_PLANE_BYTES];

typedef uint8_t pmt_color_table_t[16][3];

typedef uint8_t bmp_color_table_t[16][4];

typedef struct {
    uint8_t magic[2];
    uint32_t file_size;
    uint32_t reserved;
    uint32_t pixel_array_offset;
} bmp_file_header_t;

typedef struct {
    uint32_t header_size;
    uint32_t width;
    uint32_t height;
    uint16_t color_planes;
    uint16_t bits_per_pixel;
    uint32_t compression;
    uint32_t image_size;
    uint32_t horizontal_ppm;
    uint32_t vertical_ppm;
    uint32_t colors;
    uint32_t important_colors;
} bmp_info_header_t;

typedef uint8_t vga_palette_t[256][3];

#pragma pack(pop)


/* Constants */

#define BMP_HEADER_SIZE (sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t) + sizeof(bmp_color_table_t))

const bmp_file_header_t bmp_file_header = {
    {'B', 'M'},
    BMP_HEADER_SIZE + BMP_PIXEL_ARRAY_SIZE,
    0,
    BMP_HEADER_SIZE
};

const bmp_info_header_t bmp_info_header = {
    sizeof(bmp_info_header_t),
    PMT_PIXELS_PER_ROW,
    /* Negative value results in BMP pixel array being read from top to bottom. */
    -PMT_ROWS,
    1,
    PMT_PLANES,
    0,
    0,
    0,
    0,
    (1 << PMT_PLANES),
    0
};

/* Read PMT raw byte group data into a buffer. */
int pmt_read_group(FILE *fh, uint8_t *buf, int buf_size) {
    uint16_t group_size;

    int bytes_read = fread(&group_size, 1, 2, fh);
    if (bytes_read != 2) {
        printf("ERROR: failed to read size for the group.\n");
        return -1;
    }

    if (group_size > buf_size) {
        printf("ERROR: group data (%d bytes) does not fit in buffer (%d bytes).\n", group_size,
               buf_size);
        return -1;
    }

    bytes_read = fread(buf, 1, group_size, fh);
    if (bytes_read != group_size) {
        printf("ERROR: failed to read %d group data bytes.\n", group_size);
        return -1;
    }

    return bytes_read;
}

/* Decode raw group data. */
int pmt_decode_group(uint8_t *raw_group_buf, int raw_group_size, uint8_t *decoded_group_buf,
                     int decoded_group_buf_size) {
    uint8_t *src = raw_group_buf;
    uint8_t *dst = decoded_group_buf;

    while((src - raw_group_buf) < raw_group_size) {
        if (*src & 0x80) {
            /* Clearing the most significant bit gives the run length. */
            uint8_t run_length = *src & 0x7f;
            /* Next byte is run byte. */
            src++;
            if ((src - raw_group_buf) > raw_group_size) {
                printf("ERROR: attempted to read past end of input data.\n");
                return -1;
            }
            if ((dst + run_length - decoded_group_buf) > decoded_group_buf_size) {
                printf("ERROR: attempted to write past end of output buffer.\n");
                return -1;
            }
            memset(dst, *src, run_length);

            src++;
            dst += run_length;
        } else {
            /* Byte value is the number of bytes to copy verbatim to output buffer. */
            uint8_t copy_count = *src;
            src++;
            if ((src - raw_group_buf) > raw_group_size) {
                printf("ERROR: attempted to read past end of input data.\n");
                return -1;
            }
            if ((dst + copy_count - decoded_group_buf) > decoded_group_buf_size) {
                printf("ERROR: attempted to write past end of output buffer.\n");
                return -1;
            }
            memcpy(dst, src, copy_count);

            src += copy_count;
            dst += copy_count;
        }
    }
 
    int decoded_bytes = dst - decoded_group_buf;
    if (decoded_bytes != decoded_group_buf_size) {
        printf("ERROR: decoded %d bytes, expected %d\n", decoded_bytes, decoded_group_buf_size);
        return -1;
    } else {
        return decoded_bytes;
    }
}

/* Extracts the value of the PMT plane pixel at the specified bit position within the byte. */
uint8_t pmt_plane_pixel(uint8_t pmt_byte, uint8_t pmt_pixel_idx) {
    uint8_t bit_mask = 1 << pmt_pixel_idx;
    return (pmt_byte & bit_mask) >> pmt_pixel_idx;
}

/* Computes BMP pixel value for a given pixel in PMT row. */
uint8_t bmp_pixel(pmt_row_t *pmt_row, int pmt_row_pixel_idx) {
    /* Position of the byte containing the specified pixel in the PMT row. */
    int pmt_byte_idx = pmt_row_pixel_idx / 8;
    /* Position of the pixel bit within the byte. */
    uint8_t pmt_pixel_idx = 7 - (pmt_row_pixel_idx - pmt_byte_idx * 8);

    uint8_t result = 0;
    for (int pmt_plane = 0; pmt_plane < PMT_PLANES; pmt_plane++) {
        uint8_t pmt_byte = (*pmt_row)[pmt_plane][pmt_byte_idx];
        result += pmt_plane_pixel(pmt_byte, pmt_pixel_idx) << pmt_plane;
    }

    return result;
}

/* Convert PMT byte group to corresponding BMP bytes. */
uint8_t *convert_group_to_bmp(uint8_t *pmt_group_start, uint8_t *bmp_group_start) {
    uint8_t *bmp_ptr;

    for (int row = 0; row < PMT_ROWS_PER_GROUP; row++) {
        pmt_row_t *pmt_row = (pmt_row_t *) (pmt_group_start + row * PMT_DECODED_ROW_BYTES);
        bmp_ptr = bmp_group_start + row * BMP_ROW_BYTES;

        /* Resulting BMP byte holding information about 2 pixels, 4 bits each. */
        uint8_t bmp_byte = 0;
        for (int pmt_pixel_idx = 0; pmt_pixel_idx < PMT_PIXELS_PER_ROW; pmt_pixel_idx++) {
            uint8_t bmp_pixel_val = bmp_pixel(pmt_row, pmt_pixel_idx);
            if (pmt_pixel_idx % 2) {
                /* Odd pixel, needs to be placed into lower half-byte of BMP byte. */
                bmp_byte += bmp_pixel_val;
                *bmp_ptr = bmp_byte;
                bmp_ptr++;
                bmp_byte = 0;
            } else {
                /* Even pixel, needs to be placed into upper half-byte of BMP byte. */
                bmp_byte += bmp_pixel_val << 4;
            }
        }
    }
    return bmp_ptr;
}

/* Convert PMT color table to BMP color table. */
void pmt_to_bmp_color_table(pmt_color_table_t *pmt_color_table,
                            bmp_color_table_t *bmp_color_table) {
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 3; c++) {
            /* PMT color table contains 6-bit R/G/B values, BMP color table expects */
            /* 8-bit B/G/R values with a terminating zero byte.                     */
            (*bmp_color_table)[i][c] = (*pmt_color_table)[i][2-c] << 2;
        }
        (*bmp_color_table)[i][3] = 0;
    }
}

/* Convert data from PMT file to BMP data. */
uint8_t *pmt_to_bmp(char *pmt_file_name, uint8_t *bmp_buf, bmp_color_table_t *bmp_color_table) {
    uint8_t raw_group_buf[PMT_DECODED_GROUP_BYTES];
    uint8_t decoded_group_buf[PMT_DECODED_GROUP_BYTES];
    uint8_t *end_ptr;

    FILE *in = fopen(pmt_file_name, "r");
    if (!in) {
        printf("ERROR: failed to open file %s for reading.\n", pmt_file_name);
        return NULL;
    }

    for(int i = 0; i < PMT_GROUPS; i++) {
        int group_bytes_read = pmt_read_group(in, raw_group_buf, PMT_DECODED_GROUP_BYTES);
        if (group_bytes_read < 0) {
            printf("ERROR: failed to read group %d from file %s\n", i, pmt_file_name);
            return NULL;
        }

        int decoded_bytes = pmt_decode_group(raw_group_buf, group_bytes_read, decoded_group_buf, PMT_DECODED_GROUP_BYTES);
        if (decoded_bytes < 0) {
            printf("ERROR: failed to decode group %d from file %s\n", i, pmt_file_name);
            return NULL;
        }

        end_ptr = convert_group_to_bmp(decoded_group_buf, bmp_buf + i * BMP_GROUP_BYTES);
    }

    pmt_color_table_t pmt_color_table;
    /* Skip 16 bytes of the footer to get to the raw color table. */
    fseek(in, 16, SEEK_CUR);
    /* Table of 16 colors, each specified as three 6-bit R/G/B values. */
    int color_table_bytes_read = fread(&pmt_color_table, 1, sizeof(pmt_color_table_t), in);
    if (color_table_bytes_read != sizeof(pmt_color_table_t)) {
        printf("ERROR: unexpected color table bytes read (%d)\n", color_table_bytes_read);
        return NULL;
    }
    pmt_to_bmp_color_table(&pmt_color_table, bmp_color_table);

    return end_ptr;
}


int main(int argc, char **argv) {

    uint8_t bmp_pixel_array[BMP_PIXEL_ARRAY_SIZE];
    bmp_color_table_t bmp_color_table;

    if (argc < 3) {
        printf("ERROR: insufficient arguments.\n");
        printf("Usage: %s input_file_name output_file_name\n", argv[0]);
        return 1;
    }

    uint8_t *out_ptr = pmt_to_bmp(argv[1], bmp_pixel_array, &bmp_color_table);
    if ((out_ptr - bmp_pixel_array) != BMP_PIXEL_ARRAY_SIZE) {
        printf("ERROR: unexpected number of bytes in pixel array.\n");
        return 1;
    }

    FILE *out = fopen(argv[2], "w");
    fwrite(&bmp_file_header, 1, sizeof(bmp_file_header), out);
    fwrite(&bmp_info_header, 1, sizeof(bmp_info_header), out);
    fwrite(&bmp_color_table, 1, sizeof(bmp_color_table), out);
    fwrite(bmp_pixel_array, 1, BMP_PIXEL_ARRAY_SIZE, out);
    fclose(out);
}
