/*
 *  Copyright (C) 2010, 2019, 2021 D. R. Commander.  All Rights Reserved.
 *  Copyright (C) 2010 University Corporation for Atmospheric Research.
 *                     All Rights Reserved.
 *  Copyright (C) 2002-2003 Constantin Kaplinsky.  All Rights Reserved.
 *  Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 *  USA.
 */

/*
 *  vncpasswd:  A standalone program that gets and verifies a password,
 *              encrypts it, and stores it to a file.  Optionally, it does
 *              the same for a second (view-only) password.  Always ignore
 *              anything after 8 characters, since this is what Solaris
 *              getpass() does anyway.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include "vncauth.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#ifdef UseDevUrandom
#define URANDOM_PATH "/dev/urandom"

#include <stdint.h>
#include <fcntl.h>
#else
#include <sys/time.h>
#endif


static void usage(char *argv[]);
static char *getenv_safe(char *name, size_t maxlen);
static void mkdir_and_check(char *dirname, int be_strict);
static int read_password(char *result);
static int ask_password(char *result);

int alsoView;

int otp;
int otpClear;
char *displayname;


static int DoOTP(void)
{
  unsigned int full;
  unsigned int view = 0;
  Display *dpy;
  Atom prop;
  int len;
  char buf[MAXPWLEN + 1];
  char bytes[MAXPWLEN * 2];
#ifdef UseDevUrandom
  int fd;
#endif

  if ((dpy = XOpenDisplay(displayname)) == NULL) {
    fprintf(stderr, "unable to open display \"%s\"\n",
            XDisplayName(displayname));
    return(1);
  }

  prop = XInternAtom(dpy, "VNC_OTP", True);
  if (prop == None) {
    fprintf(stderr, "The X display \"%s\" does not support VNC one-time passwords\n",
            XDisplayName(displayname));
    return(1);
  }

  if (otpClear) {
    len = 0;

  } else {
#ifdef UseDevUrandom
    if ((fd = open(URANDOM_PATH, O_RDONLY, 0)) < 0) {
      perror(URANDOM_PATH);
      return(1);
    }

    if (
      (read(fd, &full, sizeof(full)) != sizeof(full)) ||
      (alsoView && (read(fd, &view, sizeof(view)) != sizeof(view)))
    ) {
      fprintf(stderr, "Could not read random number from %s\n", URANDOM_PATH);
      return(1);
    }

    close(fd);

#else
    struct timeval now;

    gettimeofday(&now, NULL);
    srandom((unsigned int)now.tv_sec + (unsigned int)now.tv_usec);
    full = random();
    if (alsoView)
      view = random();
#endif

    snprintf(buf, sizeof(buf), "%08u", full);
    memcpy(&bytes[0], buf, MAXPWLEN);
    fprintf(stderr, "Full control one-time password: %.*s\n", MAXPWLEN,
            &bytes[0]);
    len = MAXPWLEN;
    if (alsoView) {
      snprintf(buf, sizeof(buf), "%08u", view);
      memcpy(&bytes[MAXPWLEN], buf, MAXPWLEN);
      fprintf(stderr, "View-only one-time password: %.*s\n", MAXPWLEN,
              &bytes[MAXPWLEN]);
      len = MAXPWLEN * 2;
    }
  }

#ifdef notdef
  if (len >= MAXPWLEN)
    vncEncryptPasswd(&bytes[0], &bytes[0]);

  if (len == MAXPWLEN * 2)
    vncEncryptPasswd(&bytes[MAXPWLEN], &bytes[MAXPWLEN]);
#endif

  XChangeProperty(dpy, DefaultRootWindow(dpy), prop, XA_STRING, 8,
                  PropModeReplace, (unsigned char *)bytes, len);
  memset(bytes, 0, sizeof(bytes));
  XCloseDisplay(dpy);
  return 0;
}


int addUser;
int userList;
char *user;

#define MAXUSERLEN  63

static int DoUserList(void)
{
  Display *dpy;
  Atom prop;
  int len;
  char bytes[MAXUSERLEN + 1];

  if ((user == NULL) || ((len = strlen(user)) == 0)) {
    fprintf(stderr, "missing the username!");
    return 1;
  }

  if (len > MAXUSERLEN) {
    fprintf(stderr, "username is too large");
    return 1;
  }

  if ((dpy = XOpenDisplay(displayname)) == NULL) {
    fprintf(stderr, "unable to open display \"%s\"\n",
            XDisplayName(displayname));
    return 1;
  }

  prop = XInternAtom(dpy, "VNC_ACL", True);
  if (prop == None) {
    fprintf(stderr, "The X server \"%s\" does not support VNC user access control lists\n",
            XDisplayName(displayname));
    return 1;
  }

  bytes[0] = addUser | (alsoView ? 0x10 : 0x00);
  memcpy(&bytes[1], user, len);
  XChangeProperty(dpy, DefaultRootWindow(dpy), prop, XA_STRING, 8,
                  PropModeReplace, (unsigned char *)bytes, len + 1);
  XCloseDisplay(dpy);
  return 0;
}


int main(int argc, char *argv[])
{
  int read_from_stdin = 0;
  int make_directory = 1;
  int check_strictly = 0;
  char passwd1[9];
  char passwd2[9];
  char *passwd2_ptr;
  char yesno[2];
  char passwdDir[256];
  char passwdFile[263];
  int i;

  sprintf(passwdDir, "%s/.vnc", getenv_safe("HOME", 240));
  sprintf(passwdFile, "%s/passwd", passwdDir);

  for (i = 1; i < argc; i++) {
    if (argv[i][0] != '-')
      break;

    switch (argv[i][1]) {
      case 'd':
        if (strcmp("-display", argv[i]))
          usage(argv);

        if (++i >= argc)
          usage(argv);

        displayname = argv[i];
        break;

      case 'c':
        make_directory = 0;
        otp = 1;  otpClear = 1;
        break;

      case 'o':
        make_directory = 0;
        otp = 1;
        break;

      case 'a':
        if (++i >= argc)
          usage(argv);

        user = argv[i];
        make_directory = 0;
        userList = 1;
        addUser = 1;
        break;

      case 'r':
        if (++i >= argc)
          usage(argv);

        user = argv[i];
        make_directory = 0;
        userList = 1;
        addUser = 0;
        break;

      case 'f':
        strcpy(passwdFile, "-");
        read_from_stdin = 1;
        make_directory = 0;
        check_strictly = 0;
        break;

      case 't':
        sprintf(passwdDir, "/tmp/%s-vnc", getenv_safe("USER", 32));
        sprintf(passwdFile, "%s/passwd", passwdDir);
        read_from_stdin = 0;
        make_directory = 1;
        check_strictly = 1;
        break;

      case 'v':
        alsoView = 1;
        break;

      default:
        usage(argv);
        break;
    }
  }

  if (otp) {
    if (read_from_stdin) {
      fprintf(stderr, "Error: -f is incompatible with -o\n");
      exit(1);
    }

    if (!strncmp(passwdDir, "/tmp", 4)) {
      fprintf(stderr, "Error: -t is incompatible with -o\n");
      exit(1);
    }

    if (userList) {
      fprintf(stderr, "Error: -a and -r are incompatible with -o\n");
      exit(1);
    }

    exit(DoOTP());
  }

  if (userList) {
    if (read_from_stdin) {
      fprintf(stderr, "Error: -f is incompatible with -a and -r\n");
      exit(1);
    }

    if (!strncmp(passwdDir, "/tmp", 4)) {
      fprintf(stderr, "Error: -t is incompatible with -a and -r\n");
      exit(1);
    }

    if (otp) {
      fprintf(stderr, "Error: -o is incompatible with -a and -r\n");
      exit(1);
    }

    exit(DoUserList());
  }

  if (i != argc) {
    if (strlen(argv[i]) > sizeof(passwdFile) - 1) {
      fprintf(stderr, "Error: file name too long\n");
      exit(1);
    }
    strcpy(passwdFile, argv[i]);
    if (read_from_stdin) {
      fprintf(stderr, "Error: cannot specify filename with -f\n");
      exit(1);
    }
    read_from_stdin = 0;
    make_directory = 0;
    check_strictly = 0;
  }

  if (make_directory) {
    fprintf(stderr, "Using password file %s\n", passwdFile);
    mkdir_and_check(passwdDir, check_strictly);
  }

  passwd2_ptr = NULL;

  if (read_from_stdin) {

    /* Read one or two passwords from stdin */
    if (!read_password(passwd1)) {
      fprintf(stderr, "Could not read password\n");
      exit(1);
    }
    if (read_password(passwd2)) {
      passwd2_ptr = passwd2;
    }

  } else {

    /* Ask the primary (full-control) password. */
    if (!ask_password(passwd1)) {
      exit(1);
    }

    /* Optionally, ask the second (view-only) password. */
    if (alsoView) {
      fprintf(stderr, "Enter the view-only password\n");

    } else {
      /* FIXME: Is it correct to read from stdin here? */
      fprintf(stderr, "Would you like to enter a view-only password (y/n)? ");
      if (fgets(yesno, 2, stdin) != NULL && strchr("Yy", yesno[0]) != NULL)
        alsoView = 1;
    }

    if (alsoView && ask_password(passwd2)) {
      passwd2_ptr = passwd2;
    }
  }

  /* Actually write the passwords. */
  if (!vncEncryptAndStorePasswd2(passwd1, passwd2_ptr, passwdFile)) {
    memset(passwd1, 0, strlen(passwd1));
    memset(passwd2, 0, strlen(passwd2));
    fprintf(stderr, "Cannot write password file %s\n", passwdFile);
    exit(1);
  }

  /* Zero the memory. */
  memset(passwd1, 0, strlen(passwd1));
  memset(passwd2, 0, strlen(passwd2));
  return 0;
}


static void usage(char *argv[])
{
  fprintf(stderr, "usage: %s [-v] [FILE]\n", argv[0]);
  fprintf(stderr, "       %s -f\n", argv[0]);
  fprintf(stderr, "       %s -t [-v]\n", argv[0]);
  fprintf(stderr, "       %s -o [-v] [-display VNC-DISPLAY]\n", argv[0]);
  fprintf(stderr, "       %s -c [-display VNC-DISPLAY]\n", argv[0]);
  fprintf(stderr, "       %s -a USER [-v] [-display VNC-DISPLAY]\n", argv[0]);
  fprintf(stderr, "       %s -r USER [-display VNC-DISPLAY]\n", argv[0]);
  exit(1);
}


static char *getenv_safe(char *name, size_t maxlen)
{
  char *result;

  result = getenv(name);
  if (result == NULL) {
    fprintf(stderr, "Error: no %s environment variable\n", name);
    exit(1);
  }
  if (strlen(result) > maxlen) {
    fprintf(stderr, "Error: %s environment variable string too long\n", name);
    exit(1);
  }
  return result;
}


/*
 * Check if the specified vnc directory exists, create it if
 * necessary, and perform a number of sanity checks.
 */

static void mkdir_and_check(char *dirname, int be_strict)
{
  struct stat stbuf;

  if (lstat(dirname, &stbuf) != 0) {
    if (errno != ENOENT) {
      fprintf(stderr, "lstat() failed for %s: %s\n", dirname, strerror(errno));
      exit(1);
    }
    fprintf(stderr, "VNC directory %s does not exist, creating.\n", dirname);
    if (mkdir(dirname, S_IRWXU) == -1) {
      fprintf(stderr, "Error creating directory %s: %s\n", dirname,
              strerror(errno));
      exit(1);
    }
  }

  if (lstat(dirname, &stbuf) != 0) {
    fprintf(stderr, "Error in lstat() for %s: %s\n", dirname, strerror(errno));
    exit(1);
  }
  if (!S_ISDIR(stbuf.st_mode)) {
    fprintf(stderr, "Error: %s is not a directory\n", dirname);
    exit(1);
  }
  if (stbuf.st_uid != getuid()) {
    fprintf(stderr, "Error: bad ownership on %s\n", dirname);
    exit(1);
  }
  if (be_strict && ((S_IRWXG | S_IRWXO) & stbuf.st_mode)) {
    fprintf(stderr, "Error: bad access modes on %s\n", dirname);
    exit(1);
  }
}


/*
 * Read a password from stdin.  The password is terminated either by an
 * end of line, or by the end of stdin data.  Return 1 on success, 0 on
 * error. On success, the password will be stored in the specified
 * 9-byte buffer.
 */

static int read_password(char *result)
{
  char passwd[256];
  char *ptr;

  /* Try to read the password. */
  if (fgets(passwd, 256, stdin) == NULL)
    return 0;

  /* Remove the newline if present. */
  ptr = strchr(passwd, '\n');
  if (ptr != NULL)
    *ptr = '\0';

  /* Truncate if necessary. */
  if (strlen(passwd) > 8) {
    memset(passwd + 8, 0, strlen(passwd) - 8);
    fprintf(stderr, "Warning: password truncated to the length of 8.\n");
  }

  /* Save the password and zero our copies. */
  strcpy(result, passwd);
  memset(passwd, 0, strlen(passwd));

  return 1;
}


/*
 * Ask a password, check its length and ask to confirm it once more.
 * Return 1 on success, 0 on error.  On success, the password will be
 * stored in the specified 9-byte buffer.
 */

static int ask_password(char *result)
{
  char *passwd;
  char passwd_copy[9];

  while (1) {
    passwd = getpass("Password: ");
    if (!passwd) {
      fprintf(stderr, "Can't get password: not a tty?\n");
      return 0;
    }
    if (strlen(passwd) < 6) {
      fprintf(stderr, "Password too short\n");
      return 0;
    }
    if (strlen(passwd) > 8) {
      memset(passwd + 8, 0, strlen(passwd) - 8);
      fprintf(stderr, "Warning: password truncated to the length of 8.\n");
    }

    strcpy(passwd_copy, passwd);

    passwd = getpass("Verify:   ");
    if (strlen(passwd) > 8)
      memset(passwd + 8, 0, strlen(passwd) - 8);

    if (strcmp(passwd, passwd_copy) == 0)
      break;                    /* success */

    fprintf(stderr, "Passwords do not match. Please try again.\n\n");
  }

  /* Save the password and zero our copies. */
  strcpy(result, passwd);
  memset(passwd, 0, strlen(passwd));
  memset(passwd_copy, 0, strlen(passwd_copy));

  return 1;
}
