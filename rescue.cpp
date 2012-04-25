/*
 * Deleted/lost file recovery program
 
 * Copyright (C) 2010-2012 Ryan Lothian
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */


/* Usage: ./rescue <filename>
 *
 * Warning: This code mmaps the whole file into memory, so for files more than
 *          a couple of GB, it requires a 64-bit processor.
 *
 * Note:    Fragments (files found) get written to the current directory, and
 *          there may be several thousand of them.
 *
 * Note:    This program writes Unix commands to stdout. These should be run to
 *          cut down on false positives. They may be run concurrently with the
 *          program (pipe to bash).
 */
 
#include <stdio.h>
#include <stdint.h>
#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <iomanip>

/*
 * We output stuff we find as files named: "fragment-" + id + "." + extension
 * g_fragment_id records the last id output.
 */
static        uint64_t   g_fragment_id         = 0;


/*
 * g_start is the address of the start of the file/disk in memory. We can use 
 * it to work out byte offsets for debug messages, e.g."JPEG header found at 
 * byte 1234".
 */
static const  uint8_t   *g_start               = NULL;


/*
 * Used by the ASCII file finding code to prevent it reporting the same file
 * several times.
 */
static const  uint8_t   *g_ascii_ignore_before = NULL;


/*
 * check_if_bytes_match
 *
 * Check whether (mem[i] & mask[i]) == pattern[i] for each byte in the pattern.
 *
 * mem:          Current position in the file.
 * end:          1 byte past the last byte of the file.
 * pattern_size: Number of bytes in the pattern.
 * pattern:      Pattern bytes.
 * mask:         Mask bytes.
 */
static inline bool
check_if_bytes_match(const uint8_t         *mem,
                     const uint8_t * const  end,
                     const unsigned int     pattern_size,
                     const uint8_t         *pattern,
                     const uint8_t         *mask)
{
    unsigned int i = 0;

    while (i < pattern_size && &mem[i] != end &&
           (mem[i] & mask[i]) == pattern[i]) {
        i++;
    }

    return (i == pattern_size);
}
                   

/*
 * jpeg_is_header
 *
 * Check if a JFIF or EXIF JPEG header at the current position.
 *
 * mem: Current position in the file.
 * end: 1 byte past the last byte of the file.
 */
static inline bool
jpeg_is_header(const uint8_t *mem,
               const uint8_t *const end)
{
    /*
     * JPEG JFIF files have 10 fixed bytes in the first 12. Check those bytes.
     * JPEG EXIF files also have 10 fixed bytes, but with different values.
     */
     
    const unsigned int  jpeg_pattern_size      = 12;
    
    const uint8_t       jfif_pattern[jpeg_pattern_size] = {
        0xFF,   0xD8,   0xFF,  0xE0,
        0/*?*/, 0/*?*/, 0x4A,  0x46,
        0x49,   0x46,   0x00,  0x01};

    const uint8_t       exif_pattern[jpeg_pattern_size] = {
        0xFF,   0xD8,   0xFF,  0xE1,
        0/*?*/, 0/*?*/, 0x45,  0x78,
        0x69,   0x66,   0x00,  0x00};
        
    const uint8_t       jpeg_mask[jpeg_pattern_size] = {
        0xFF,   0xFF,   0xFF,  0xFF,
        0,      0,      0xFF,  0xFF,
        0xFF,   0xFF,   0xFF,  0xFF};
    

    return check_if_bytes_match(mem, end, jpeg_pattern_size, jfif_pattern, 
                                jpeg_mask) ||
           check_if_bytes_match(mem, end, jpeg_pattern_size, exif_pattern, 
                                jpeg_mask);    
}


/*
 * jpeg_find_footer
 *
 * Returns one byte past the end of the jpeg file starting at mem, or NULL if
 * no end was found.
 *
 * mem: The position of the header.
 * end: One byte past the end of the file.
 */
static inline const uint8_t *
jpeg_find_footer(const uint8_t *mem,
                 const uint8_t *const end)
{
    /*
     * We assume JPEGs are never more than 40MB.
     */
    const uint64_t max_length_bytes = 40 * 1024 * 1024; // 40 MB
    const uint8_t *max_mem          = end - 2;
    
    if (max_mem > mem + max_length_bytes) {
        max_mem = mem + max_length_bytes;
    }
     
    /*
     * JFIF and EXIF JPEGs both end with FF D9. However, if followed by FF E1,
     * this (usually) means the file has more parts remaining.
     */
    while (mem <= max_mem) {
        if (mem[0] == 0xFF && mem[1] == 0xD9) {
            if (mem + 3 >= end || mem[2] != 0xFF || mem[3] != 0xE1) {            
                return &mem[2];
            }
        }
        
        mem++;         
    }

    return NULL;
}


/*
 * write_fragment
 *
 * Writes a file fragment to disk.
 *
 * start:     The start of the file fragment.
 * end:       One byte past the end of the file fragment.
 * extension: The file extension, e.g. "txt".
 */
static inline void
write_fragment(const uint8_t *const  start,
               const uint8_t *const  end,
               const char    *const  extension)
{
    /*
     * Work out the file name.
     */
    char filename[256];

    g_fragment_id++;
    snprintf(filename, 256, "%s-fragment-%llu.%s",
             extension, 
             (unsigned long long)g_fragment_id,
             extension);

    /*
     * Write the file to disk.
     */
    FILE *f = fopen(filename, "w");
    fwrite(start, 1, end - start, f);
    fclose(f);

    std::cout << "gzip " << filename << "\n";
    /*
     * Output debug message.
     */
    std::cerr << "Wrote " << filename << " (" << (end - start) << " bytes)\n";
}


/*
 * jpeg_rescue
 *
 * Looks for a JPEG file starting at cur. If found, writes it to disk.
 *
 * cur: Current position in file in memory.
 * end: One byte past the end of the file in memory.
 */
static inline void
jpeg_rescue(const uint8_t *const cur,
            const uint8_t *const end)
{
    /*
     * To do: stop examining in the middle of JPEG files that have already
     *        been found.     
     */
         
    if (jpeg_is_header(cur, end)) {
        std::cerr << "JPEG: found header at byte "
                  << (uint64_t)(cur - g_start) << "\n";

        const uint8_t *const jpeg_end = jpeg_find_footer(cur, end);
        if (jpeg_end == NULL) {
            std::cerr << "JPEG: footer not found!\n";
        } else {
            std::cerr << "JPEG: footer found\n";
            write_fragment(cur, jpeg_end, "jpg");
        }
    }
}


/*
 * ascii_rescue
 *
 * Looks for a ASCII file starting at cur. If found, writes it to disk.
 *
 * cur: Current position in file in memory.
 * end: One byte past the end of the file in memory.
 */
static inline void
ascii_rescue(const uint8_t *cur,
             const uint8_t * const end)
{
    const uint8_t * const ascii_start = cur;
    const uint64_t        min_size_bytes = 1024;

    /*
     * If this byte has already been examined, ignore it.
     */
    if (cur < g_ascii_ignore_before) {
        return;
    }

    /*
     * Keep going while the character is within the printable ASCII range.
     */
    while (*cur >= 0x20 && *cur <= 0x7E && cur < end) {
        cur++;
    }

    /*
     * If we found sufficiently many, this is probably an ASCII text file.
     */
    if (cur - ascii_start >= min_size_bytes) {
        std::cerr << "ASCII: " << (uint64_t)(cur - ascii_start)
                  << " bytes of text found\n";
        write_fragment(ascii_start, cur, "txt");
    }
    g_ascii_ignore_before = cur;
}

/*
 * main
 *
 * See README for a description of this program.
 *
 * Returns 0 on success, 1 on failure.
 *           
 * argc:  Should be 2 (see 'Usage' below).
 * argv:  Program name, filename.
 */
int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << 
            "rescue - Copyright (C) Ryan Lothian 2010-2012\n" <<
            "This program comes with ABSOLUTELY NO WARRANTY; for details see LICENSE.\\nn"
            
        std::cerr << "Usage: " << argv[0] << " <filename>\n";
        return 1;
    }

    /*
     * Get the file into memory (requires 64-bit machine for files larger than,
     * say, 1 or 2 GB).
     */
    int fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
        std::cerr << "Could not open file "<< argv[1] << "\n";
        return 1;
    }

    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        std::cerr << "Could not stat file\n";
        return 1;
    }

    g_start = (const uint8_t*)mmap(0, sb.st_size, PROT_READ, MAP_SHARED, fd, 
                                   0);

    if (g_start == MAP_FAILED) {
        std::cerr << "Could not mmap file\n";
        return 1;
    }

    std::cerr << "Successfully opened file. Size: "
              << (sb.st_size / (1024UL * 1024UL)) << " MB\n";

    const uint8_t          *cur              = g_start;
    const uint8_t * const   end              = cur + sb.st_size;
    uint64_t                percent_complete = 0;

    /*
     * Step through one byte at a time, checking for JPEG and ASCII files.
     */
    while (cur != end) {
        /*
         * Print a percentage completion, overwriting the previous percentage
         * each time.
         */
        const uint64_t new_percent = (cur - g_start) * 1000UL / sb.st_size;
        if (new_percent > percent_complete) {
            percent_complete = new_percent;
            std::cerr << std::fixed << std::setprecision(1) << 
                         (percent_complete / 10.0) << "%  \r";
        }
        
        jpeg_rescue(cur, end);
        ascii_rescue(cur, end);
        cur++;
    }
 
    /*
     * At this point, the entire file has been read, and all found fragments
     * written to disk.
     */   
    std::cerr << "Complete.  \n";

    return 0;
}
