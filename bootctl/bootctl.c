/*
 * Copyright (c) 2017, Mellanox Technologies Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE  // asprintf
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/major.h>

/* Boot FIFO constants */
#define BOOT_FIFO_ADDR 0x0408
#define SEGMENT_HEADER_LEN 8
#define MAX_SEG_LEN ((1 << 20) - SEGMENT_HEADER_LEN)

void die(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  putc('\n', stderr);
  exit(1);
}

#ifndef OUTPUT_ONLY

#include <linux/mmc/ioctl.h>

/*
 * The Linux MMC driver doesn't export its ioctl command values, so we
 * copy them from include/linux/mmc/mmc.h and include/linux/mmc/core.h.
 */
#define MMC_SWITCH                6   /* ac   [31:0] See below   R1b */
#define MMC_SEND_EXT_CSD          8   /* adtc                    R1  */
#define MMC_RSP_PRESENT	(1 << 0)
#define MMC_RSP_CRC	(1 << 2)		/* expect valid crc */
#define MMC_RSP_BUSY	(1 << 3)		/* card may send busy */
#define MMC_RSP_OPCODE	(1 << 4)		/* response contains opcode */
#define MMC_RSP_SPI_S1	(1 << 7)		/* one status byte */
#define MMC_RSP_SPI_BUSY (1 << 10)		/* card may send busy */
#define MMC_RSP_SPI_R1	(MMC_RSP_SPI_S1)
#define MMC_RSP_R1	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R1B	(MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE|MMC_RSP_BUSY)
#define MMC_RSP_SPI_R1B	(MMC_RSP_SPI_S1|MMC_RSP_SPI_BUSY)
#define MMC_CMD_AC	(0 << 5)
#define MMC_CMD_ADTC	(1 << 5)
#define MMC_SWITCH_MODE_WRITE_BYTE	0x03	/* Set target to value */
#define EXT_CSD_CMD_SET_NORMAL		(1<<0)
#define EXT_CSD_PART_CONFIG		179	/* R/W */

/* Program constants */
#define EMMC_BLOCK_SIZE 512
#define SYS_PATH "/sys/bus/platform/drivers/mlx-bootctl"
#define SECOND_RESET_ACTION_PATH SYS_PATH "/second_reset_action"
#define POST_RESET_WDOG_PATH SYS_PATH "/post_reset_wdog"

/* Program variables */
const char *mmc_path = "/dev/mmcblk0";

/* Run an MMC_IOC_CMD ioctl on mmc_path */
void mmc_command(struct mmc_ioc_cmd *idata)
{
  static int mmc_fd = -1;
  if (mmc_fd < 0)
  {
    mmc_fd = open(mmc_path, O_RDWR);
    if (mmc_fd < 0)
      die("%s: %m", mmc_path);
  }
  if (ioctl(mmc_fd, MMC_IOC_CMD, idata) < 0)
    die("%s: mmc ioctl: %m", mmc_path);
}

/* Return the current partition (0 or 1) that we will boot from. */
int get_boot_partition(void)
{
  static uint8_t ext_csd[EMMC_BLOCK_SIZE]
    __attribute__((aligned(EMMC_BLOCK_SIZE))) = { 0 };
  struct mmc_ioc_cmd idata = {
    .write_flag = 0,
    .opcode = MMC_SEND_EXT_CSD,
    .arg = 0,
    .flags = MMC_RSP_SPI_R1 | MMC_RSP_R1 | MMC_CMD_ADTC,
    .blksz = EMMC_BLOCK_SIZE,
    .blocks = 1
  };

  mmc_ioc_cmd_set_data(idata, ext_csd);
  mmc_command(&idata);

  /* Subtract one to adjust for hardware 1-based numbering. */
  return ((ext_csd[EXT_CSD_PART_CONFIG] >> 3) & 0x7) - 1;
}

/* Set which partition to boot from. */
void set_boot_partition(int part)
{
  int value = ((part + 1) & 0x7) << 3;  /* Adjust for 1-based numbering */
  struct mmc_ioc_cmd idata = {
    .write_flag = 1,
    .opcode = MMC_SWITCH,
    .arg = ((MMC_SWITCH_MODE_WRITE_BYTE << 24) |
            (EXT_CSD_PART_CONFIG << 16) |
            (value << 8) |
            EXT_CSD_CMD_SET_NORMAL),
    .flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC
  };

  mmc_command(&idata);
}

int get_watchdog(void)
{
  FILE *f = fopen(POST_RESET_WDOG_PATH, "r");
  if (f == NULL)
    die("%s: %m", POST_RESET_WDOG_PATH);
  int watchdog;
  if (fscanf(f, "%d", &watchdog) != 1)
    die("%s: failed to read integer", POST_RESET_WDOG_PATH);
  fclose(f);
  return watchdog;
}

void set_watchdog(int interval)
{
  FILE *f = fopen(POST_RESET_WDOG_PATH, "w");
  if (f == NULL)
    die("%s: %m", POST_RESET_WDOG_PATH);
  if (fprintf(f, "%d\n", interval) < 0)
    die("%s: failed to set watchdog to '%d'", POST_RESET_WDOG_PATH, interval);
  fclose(f);
}

void set_second_reset_action(const char *action)
{
  FILE *f = fopen(SECOND_RESET_ACTION_PATH, "w");
  if (f == NULL)
    die("%s: %m", SECOND_RESET_ACTION_PATH);
  if (fprintf(f, "%s\n", action) < 0)
    die("%s: failed to set action to '%s'", SECOND_RESET_ACTION_PATH, action);
  fclose(f);
}

void show_status(void)
{
  // Display the default boot partition
  int part = get_boot_partition();
  printf("primary: %sboot%d\n", mmc_path, part);
  printf("backup: %sboot%d\n", mmc_path, part ^ 1);

  // Display the watchdog value
  int watchdog = get_watchdog();
  printf("watchdog-swap: ");
  if (watchdog == 0)
    printf("disabled\n");
  else
    printf("%d\n", watchdog);
}

#endif  // OUTPUT_ONLY

// Read as much as possible despite EINTR or partial reads, and die on error.
ssize_t read_or_die(const char* filename, int fd, void* buf, size_t count)
{
  ssize_t n = 0;
  while (count > 0)
  {
    ssize_t rc = read(fd, buf, count);
    if (rc < 0)
    {
      if (errno == EINTR)
        continue;
      die("%s: can't read: %m", filename);
    }
    if (rc == 0)
      break;

    n += rc;
    buf += rc;
    count -= rc;
  }
  return n;
}

// Write everything passed in despite EINTR or partial reads, and die on error.
ssize_t write_or_die(const char* filename,
                     int fd, const void* buf, size_t count)
{
  ssize_t n = count;
  while (count > 0)
  {
    ssize_t rc = write(fd, buf, count);
    if (rc < 0)
    {
      if (errno == EINTR)
        continue;
      die("%s: can't write: %m", filename);
    }
    if (rc == 0)
      die("%s: write returned zero", filename);

    buf += rc;
    count -= rc;
  }
  return n;
}

/*
 * Generate the boot stream segment header.
 *
 * is_end: 1 if this segment is the last segment of the boot code, else 0.
 * channel: The channel number to write to.
 * address: The register address to write to.
 * length: The length of this segment in bytes; max is MAX_SEG_LEN.
 *
 * We ignore endian issues here since if the tool is built natively,
 * this is likely correct anyway, and if built cross, we don't have a
 * way to know the endianness of the arm cores anyway.
 */
uint64_t gen_seg_header(int is_end, int channel, int address, size_t length)
{
  return (((is_end & 0x1UL) << 63) |
          ((channel & 0xfUL) << 45) |
          ((address & 0xfff8UL) << 29) |
          (((length + SEGMENT_HEADER_LEN) >> 3) & 0x1ffffUL));
}

void write_bootstream(const char *bootstream, const char *bootfile, int flags)
{
  int sysfd = -1;
  char *sysname;

  // Reset the force_ro setting if need be
  if (strncmp(bootfile, "/dev/", 5) == 0)
  {
    asprintf(&sysname, "/sys/block/%s/force_ro", &bootfile[5]);
    sysfd = open(sysname, O_RDWR);
    if (sysfd >= 0)
    {
      char status;
      if (read_or_die(sysname, sysfd, &status, 1) != 1)
        die("%s: unexpected EOF on read", sysname);
      if (status == '1')
      {
        char disabled = '0';
        if (lseek(sysfd, 0, SEEK_SET) != 0)
          die("%s: can't seek back to start: %m", sysname);
        write_or_die(sysname, sysfd, &disabled, 1);
      }
      else
      {
        close(sysfd);
        sysfd = -1;
        free(sysname);
        sysname = NULL;
      }
    }
    else
    {
      if (errno != ENOENT)
        die("%s: open: %m", sysname);
      printf("WARNING: No matching %s for %s\n", sysname, bootfile);
      free(sysname);
      sysname = NULL;
    }
  }

  // Copy the bootstream to the bootfile device
  int ifd = open(bootstream, O_RDONLY);
  if (ifd < 0)
    die("%s: %m", bootstream);
  int ofd = open(bootfile, O_WRONLY | flags, 0666);
  if (ofd < 0)
    die("%s: %m", bootfile);
  struct stat st;
  if (fstat(ifd, &st) < 0)
    die("%s: stat: %m", bootstream);
  size_t bytes_left = st.st_size;

  char *buf = malloc(MAX_SEG_LEN);
  if (buf == NULL)
    die("out of memory");

  // Write the bootstream header word first.  This has the byte to
  // be displayed in the rev_id register as the low 8 bits (zero for now).
  uint64_t header = 0;
  write_or_die(bootfile, ofd, &header, sizeof(header));

  while (bytes_left > 0)
  {
    size_t seg_size = (bytes_left <= MAX_SEG_LEN) ? bytes_left : MAX_SEG_LEN;
    bytes_left -= seg_size;

    // Generate the segment header.
    size_t pad_size = seg_size % 8 ? (8 - seg_size % 8) : 0;
    uint64_t segheader = gen_seg_header(bytes_left == 0, 1, BOOT_FIFO_ADDR,
                                        seg_size + pad_size);
    write_or_die(bootfile, ofd, &segheader, sizeof(segheader));

    // Copy the segment plus any padding.
    read_or_die(bootstream, ifd, buf, seg_size);
    memset(buf + seg_size, 0, pad_size);
    write_or_die(bootfile, ofd, buf, seg_size + pad_size);
  }

  if (close(ifd) < 0)
    die("%s: close: %m", bootstream);
  if (close(ofd) < 0)
    die("%s: close: %m", bootfile);
  free(buf);

  // Put back the force_ro setting if need be
  if (sysfd >= 0)
  {
    if (lseek(sysfd, 0, SEEK_SET) != 0)
      die("%s: can't seek back to start: %m", sysname);
    char enabled = '1';
    write_or_die(sysname, sysfd, &enabled, 1);
    close(sysfd);
    free(sysname);
  }
}

#ifdef OUTPUT_ONLY

int main(int argc, char **argv)
{
  static struct option long_options[] = {
    { "bootstream", required_argument, NULL, 'b' },
    { "output", required_argument, NULL, 'o' },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 }
  };
  static const char short_options[] = "b:o:h";
  static const char help_text[] =
   "syntax: mlx-bootctl [--help|-h] --bootstream|-b BFBFILE --output|-o OUTPUT";

  const char *bootstream = NULL;
  const char *output_file = NULL;
  int opt;

  while ((opt = getopt_long(argc, argv, short_options, long_options, NULL))
         != -1)
  {
    switch (opt)
    {
    case 'b':
      bootstream = optarg;
      break;

    case 'o':
      output_file = optarg;
      break;

    case 'h':
    default:
      die(help_text);
      break;
    }
  }

  if (bootstream == NULL || output_file == NULL)
    die("mlx-bootctl: Must specify --output and --bootstream");

  write_bootstream(bootstream, output_file, O_CREAT | O_TRUNC);
  return 0;
}

#else

int main(int argc, char **argv)
{
  static struct option long_options[] = {
    { "swap", no_argument, NULL, 's' },
    { "watchdog-swap", required_argument, NULL, 'w' },
    { "nowatchdog-swap", no_argument, NULL, 'n' },
    { "bootstream", required_argument, NULL, 'b' },
    { "overwrite-current", no_argument, NULL, 'c' },
    { "device", required_argument, NULL, 'd' },
    { "output", required_argument, NULL, 'o' },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 }
  };
  static const char short_options[] = "sb:d:o:h";
  static const char help_text[] =
    "syntax: bootctl [--help|-h] [--swap|-s] [--device|-d MMCFILE]\n"
    "                [--output|-o OUTPUT]\n"
    "                [--bootstream|-b BFBFILE] [--overwrite-current]\n"
    "                [--watchdog-swap interval | --nowatchdog-swap]";

  const char *watchdog_swap = NULL;
  const char *bootstream = NULL;
  const char *output_file = NULL;
  bool watchdog_disable = false;
  bool swap = false;
  int which_boot = 1;   // alternate boot partition by default
  int opt;

  while ((opt = getopt_long(argc, argv, short_options, long_options, NULL))
         != -1)
  {
    switch (opt)
    {
    case 's':
      swap = true;
      break;

    case 'w':
      watchdog_swap = optarg;
      watchdog_disable = false;
      break;

    case 'n':
      watchdog_swap = NULL;
      watchdog_disable = true;
      break;

    case 'b':
      bootstream = optarg;
      break;

    case 'c':
      which_boot = 0;    // overwrite current boot partition (dangerous)
      break;

    case 'd':
      mmc_path = optarg;
      break;

    case 'o':
      output_file = optarg;
      break;

    case 'h':
    default:
      die(help_text);
      break;
    }
  }

  if (!bootstream && !swap && watchdog_swap == NULL && !watchdog_disable)
  {
    show_status();
    return 0;
  }

  if (bootstream)
  {
    if (output_file)
    {
      // Write the bootstream to the given file, creating it if needed
      write_bootstream(bootstream, output_file, O_CREAT | O_TRUNC);
    }
    else
    {
      // Get the active partition and write to the appropriate *bootN file
      // Must save/restore boot partition, which I/O otherwise resets to zero.
      int boot_part = get_boot_partition();
      char *bootfile;
      asprintf(&bootfile, "%sboot%d", mmc_path, boot_part ^ which_boot);
      write_bootstream(bootstream, bootfile, 0);
      set_boot_partition(boot_part);
    }
  }

  if (swap)
  {
    set_boot_partition(get_boot_partition() ^ 1);
  }

  if (watchdog_swap != NULL)
  {
    // Enable reset watchdog to swap eMMC on reset after watchdog interval
    char *end;
    int watchdog = strtol(watchdog_swap, &end, 0);
    if (end == watchdog_swap || *end != '\0')
      die("watchdog-swap argument ('%s') must be an integer", watchdog_swap);
    set_watchdog(watchdog);
    set_second_reset_action("swap_emmc");
  }

  if (watchdog_disable)
  {
    // Disable reset watchdog and don't adjust reset actions at reset time
    set_watchdog(0);
    set_second_reset_action("none");
  }

  return 0;
}

#endif // OUTPUT_ONLY
