/*
 * psftp.c: (platform-independent) front end for PSFTP.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>

#include "putty.h"
#include "psftp.h"
#include "storage.h"
#include "ssh.h"
#include "sftp.h"
#include "version.h" // TG

#include "tglibcver.h"
// const char *const appname = "PSFTP"; // TG
THREADVAR TTGLibraryContext *curlibctx; // TG
THREADVAR int ThreadContextCounter=0; // TG

static int ContextCounter=0; // TG

char *appname = NORMALCODE("PSFTP") TGDLLCODE("tgputtylib"); // TG

static bool verbose = false;
bool checkpoints = false;

#define CP(x) { if (checkpoints && curlibctx) curlibctx->printmessage_callback(x,2,curlibctx); }

/*
 * Since SFTP is a request-response oriented protocol, it requires
 * no buffer management: when we send data, we stop and wait for an
 * acknowledgement _anyway_, and so we can't possibly overfill our
 * send buffer.
 */

static int psftp_connect(char *userhost, char *user, int portnumber);
static int do_sftp_init(void);
static void do_sftp_cleanup(void);
#ifdef TGDLL
static int tg_get_userpass_input(Seat *seat, prompts_t *p, bufchain *input); // TG 2019, for DLL use
#endif

/* ----------------------------------------------------------------------
 * sftp client state.
 */
#ifdef TGDLL
#define pwd (curlibctx->pwd)
#define homedir (curlibctx->homedir)
#define psftp_logctx (curlibctx->psftp_logctx)
#define backend (curlibctx->backend)
#define conf (curlibctx->conf)
#define sent_eof (curlibctx->sent_eof)
#else
char *pwd, *homedir;
static LogContext *psftp_logctx = NULL;
static Backend *backend;
Conf *conf;
bool sent_eof = false;
#endif

#ifdef _WINDOWS
// TG: emulate 64 bit tick counter
uint64_t CurrentIncrement=0;
uint64_t LastTickCount=0;

uint64_t TGGetTickCount64() // TG
{
   uint64_t LIncrement=CurrentIncrement;

   uint64_t result=GetTickCount()+LIncrement;

   if (result<LastTickCount)
   {
      result += (uint64_t) 0x100000000;
      LIncrement += (uint64_t) 0x100000000;
      CurrentIncrement=LIncrement; // thread safe - as opposed to incrementing CurrentIncrement directly
   }

   LastTickCount=result;
   return result;
}
#else
// TG: TGGetTickCount64 on Linux
#ifdef __APPLE__
#include <sys/time.h>
#endif

uint64_t TGGetTickCount64()
{
#ifdef __APPLE__
#define AVOID_CLOCK_GETTIME
#endif

#ifdef AVOID_CLOCK_GETTIME
    struct timeval tp;
    gettimeofday(&tp,NULL);
    return ((uint64_t) 1000 * tp.tv_sec) + (tp.tv_usec / 1000);
#else
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;;
#endif
}
#endif

/* ------------------------------------------------------------
 * Seat vtable.
 */

static size_t psftp_output(Seat *, bool is_stderr, const void *, size_t);
static bool psftp_eof(Seat *);

static const SeatVtable psftp_seat_vt = {
    .output = psftp_output,
    .eof = psftp_eof,
    .get_userpass_input = NORMALCODE(filexfer_get_userpass_input)TGDLLCODE(tg_get_userpass_input), // TG 2019, for DLL use
    .notify_remote_exit = nullseat_notify_remote_exit,
    .connection_fatal = console_connection_fatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = nullseat_get_ttymode,
    .set_busy_status = nullseat_set_busy_status,
    .verify_ssh_host_key = console_verify_ssh_host_key,
    .confirm_weak_crypto_primitive = console_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = console_confirm_weak_cached_hostkey,
    .is_utf8 = nullseat_is_never_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_x_display = nullseat_get_x_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = nullseat_get_window_pixel_size,
    .stripctrl_new = console_stripctrl_new,
    .set_trust_status = nullseat_set_trust_status_vacuously,
    .verbose = cmdline_seat_verbose,
    .interactive = nullseat_interactive_yes,
    .get_cursor_position = nullseat_get_cursor_position,
};
static Seat psftp_seat[1] = {{ &psftp_seat_vt }};

// TG 2019: we do not want any strip ctrl stuff, it can break UTF-8 encodings
// TG
#define with_stripctrl(varname, input)  \
    for (char *varname = input; varname;  \
         varname = NULL)

/* ----------------------------------------------------------------------
 * Manage sending requests and waiting for replies.
 */
struct sftp_packet *sftp_wait_for_reply(struct sftp_request *req)
{
    struct sftp_packet *pktin;
    struct sftp_request *rreq;

    if (!req) // TG
    {
       if (curlibctx->raise_exception_callback)
          curlibctx->raise_exception_callback("no req in sftp_wait_for_reply - not connected?",__FILE__,__LINE__,curlibctx);
       return NULL; // TG 2019
    }

    sftp_register(req);
    pktin = sftp_recv();
    if (pktin == NULL) {
        seat_connection_fatal(
            psftp_seat, "did not receive SFTP response packet from server");
    }
    rreq = sftp_find_request(pktin);
    if (rreq != req) {
        seat_connection_fatal(
            psftp_seat,
            "unable to understand SFTP response packet from server: %s",
            fxp_error());
    }
    return pktin;
}

/* ----------------------------------------------------------------------
 * Higher-level helper functions used in commands.
 */

/*
 * Attempt to canonify a pathname starting from the pwd. If
 * canonification fails, at least fall back to returning a _valid_
 * pathname (though it may be ugly, eg /home/simon/../foobar).
 */
char *canonify(const char *name)
{
    char *fullname, *canonname;
    struct sftp_packet *pktin;
    struct sftp_request *req;

    if ((name[0] == '/') || (pwd==NULL) || (strlen(pwd)==0)) // TG
    {
        fullname = dupstr(name);
    } else {
        const char *slash;
        if (pwd[strlen(pwd) - 1] == '/')
            slash = "";
        else
            slash = "/";
        fullname = dupcat(pwd, slash, name);
    }

    req = fxp_realpath_send(fullname);
    pktin = sftp_wait_for_reply(req);
    canonname = fxp_realpath_recv(pktin, req);

    if (canonname) {
		sfree(fullname);
		if (verbose) printf("Canonified %s to %s\n",name,canonname); // TG
        return canonname;
    } else {
        /*
         * Attempt number 2. Some FXP_REALPATH implementations
         * (glibc-based ones, in particular) require the _whole_
         * path to point to something that exists, whereas others
         * (BSD-based) only require all but the last component to
         * exist. So if the first call failed, we should strip off
         * everything from the last slash onwards and try again,
         * then put the final component back on.
         *
         * Special cases:
         *
         *  - if the last component is "/." or "/..", then we don't
         *    bother trying this because there's no way it can work.
         *
         *  - if the thing actually ends with a "/", we remove it
         *    before we start. Except if the string is "/" itself
         *    (although I can't see why we'd have got here if so,
         *    because surely "/" would have worked the first
         *    time?), in which case we don't bother.
         *
         *  - if there's no slash in the string at all, give up in
         *    confusion (we expect at least one because of the way
         *    we constructed the string).
         */

        int i;
        char *returnname;

        i = (int) strlen(fullname); // TG
        if (i > 2 && fullname[i - 1] == '/')
            fullname[--i] = '\0';      /* strip trailing / unless at pos 0 */
        while (i > 0 && fullname[--i] != '/');

        /*
         * Give up on special cases.
         */
        if (fullname[i] != '/' ||      /* no slash at all */
            !strcmp(fullname + i, "/.") ||      /* ends in /. */
            !strcmp(fullname + i, "/..") ||     /* ends in /.. */
            !strcmp(fullname, "/")) {
            return fullname;
        }

        /*
         * Now i points at the slash. Deal with the final special
         * case i==0 (ie the whole path was "/nonexistentfile").
         */
        fullname[i] = '\0';            /* separate the string */
        if (i == 0) {
            req = fxp_realpath_send("/");
        } else {
            req = fxp_realpath_send(fullname);
        }
        pktin = sftp_wait_for_reply(req);
        canonname = fxp_realpath_recv(pktin, req);

        if (!canonname) {
            /* Even that failed. Restore our best guess at the
             * constructed filename and give up */
            fullname[i] = '/';  /* restore slash and last component */
			if (verbose) printf("Canonifying %s failed, returning %s\n",name,fullname); // TG
            return fullname;
        }

        /*
         * We have a canonical name for all but the last path
         * component. Concatenate the last component and return.
         */
        returnname = dupcat(canonname,
                            (strendswith(canonname, "/") ? "" : "/"),
                            fullname + i + 1);
        sfree(fullname);
        sfree(canonname);
		if (verbose) printf("Canonified %s to %s\n",name,returnname); // TG
        return returnname;
    }
}

static int bare_name_compare(const void *av, const void *bv)
{
    const char **a = (const char **) av;
    const char **b = (const char **) bv;
    return strcmp(*a, *b);
}

static void not_connected(void)
{
    printf("psftp: not connected to a host\n"); // TG
}

/* ----------------------------------------------------------------------
 * The meat of the `get' and `put' commands.
 */
bool sftp_get_file(char *fname, char *outfname, bool recurse, bool restart)
{
    struct fxp_handle *fh;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    struct fxp_xfer *xfer;
    uint64_t offset;
    WFile *file;
    bool toret, shown_err = false;
    struct fxp_attrs attrs;

	CP("sftp_getf");

    /*
     * In recursive mode, see if we're dealing with a directory.
     * (If we're not in recursive mode, we need not even check: the
     * subsequent FXP_OPEN will return a usable error message.)
     */
    if (recurse) {
        bool result;

		CP("sgetfrec");
        req = fxp_stat_send(fname);
        pktin = sftp_wait_for_reply(req);
        result = fxp_stat_recv(pktin, req, &attrs);

        if (result &&
            (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) &&
            (attrs.permissions & 0040000)) {

            struct fxp_handle *dirhandle;
            size_t nnames, namesize;
            struct fxp_name **ournames;
            struct fxp_names *names;
            int i;

            /*
             * First, attempt to create the destination directory,
             * unless it already exists.
             */
            if (file_type(outfname) != FILE_TYPE_DIRECTORY &&
                !create_directory(outfname)) {
                with_stripctrl(san, outfname)
                    printf("%s: Cannot create directory\n", san);
                return false;
            }

            /*
             * Now get the list of filenames in the remote
             * directory.
             */
            req = fxp_opendir_send(fname);
            pktin = sftp_wait_for_reply(req);
            dirhandle = fxp_opendir_recv(pktin, req);

            if (!dirhandle) {
                with_stripctrl(san, fname)
                    printf("%s: unable to open directory: %s\n",
                           san, fxp_error());
                return false;
            }
            nnames = namesize = 0;
            ournames = NULL;
            while (1) {
                int i;

                req = fxp_readdir_send(dirhandle);
                pktin = sftp_wait_for_reply(req);
                names = fxp_readdir_recv(pktin, req);

                if (names == NULL) {
                    if (fxp_error_type() == SSH_FX_EOF)
                        break;
                    with_stripctrl(san, fname)
                        printf("%s: reading directory: %s\n",
                               san, fxp_error());

                    req = fxp_close_send(dirhandle);
                    pktin = sftp_wait_for_reply(req);
                    fxp_close_recv(pktin, req);

                    sfree(ournames);
                    return false;
                }
                if (names->nnames == 0) {
                    fxp_free_names(names);
                    break;
                }
                sgrowarrayn(ournames, namesize, nnames, names->nnames);
                for (i = 0; i < names->nnames; i++)
                    if (strcmp(names->names[i].filename, ".") &&
                        strcmp(names->names[i].filename, "..")) {
                        if (!vet_filename(names->names[i].filename)) {
                            with_stripctrl(san, names->names[i].filename)
                                printf("ignoring potentially dangerous server-"
                                       "supplied filename '%s'\n", san);
                        } else {
                            ournames[nnames++] =
                                fxp_dup_name(&names->names[i]);
                        }
                    }
                fxp_free_names(names);
            }
            req = fxp_close_send(dirhandle);
            pktin = sftp_wait_for_reply(req);
            fxp_close_recv(pktin, req);

            /*
             * Sort the names into a clear order. This ought to
             * make things more predictable when we're doing a
             * reget of the same directory, just in case two
             * readdirs on the same remote directory return a
             * different order.
             */
            if (nnames > 0)
                qsort(ournames, nnames, sizeof(*ournames), sftp_name_compare);

            /*
             * If we're in restart mode, find the last filename on
             * this list that already exists. We may have to do a
             * reget on _that_ file, but shouldn't have to do
             * anything on the previous files.
             *
             * If none of them exists, of course, we start at 0.
             */
            i = 0;
            if (restart) {
                while (i < nnames) {
                    char *nextoutfname;
                    bool nonexistent;
                    nextoutfname = dir_file_cat(outfname,
                                                ournames[i]->filename);
                    nonexistent = (file_type(nextoutfname) ==
                                   FILE_TYPE_NONEXISTENT);
                    sfree(nextoutfname);
                    if (nonexistent)
                        break;
                    i++;
                }
                if (i > 0)
                    i--;
            }

            /*
             * Now we're ready to recurse. Starting at ournames[i]
             * and continuing on to the end of the list, we
             * construct a new source and target file name, and
             * call sftp_get_file again.
             */
            for (; i < nnames; i++) {
                char *nextfname, *nextoutfname;
                bool retd;

                nextfname = dupcat(fname, "/", ournames[i]->filename);
                nextoutfname = dir_file_cat(outfname, ournames[i]->filename);
                retd = sftp_get_file(
                    nextfname, nextoutfname, recurse, restart);
                restart = false;       /* after first partial file, do full */
                sfree(nextoutfname);
                sfree(nextfname);
                if (!retd) {
                    for (i = 0; i < nnames; i++) {
                        fxp_free_name(ournames[i]);
                    }
                    sfree(ournames);
                    return false;
                }
            }

            /*
             * Done this recursion level. Free everything.
             */
            for (i = 0; i < nnames; i++) {
                fxp_free_name(ournames[i]);
            }
            sfree(ournames);

            return true;
        }
    }

	CP("sgetf10");
    req = fxp_stat_send(fname);
    pktin = sftp_wait_for_reply(req);
    if (!fxp_stat_recv(pktin, req, &attrs))
        attrs.flags = 0;

	CP("sgetf20");
    req = fxp_open_send(fname, SSH_FXF_READ, NULL);
	CP("sgetf21");
    pktin = sftp_wait_for_reply(req);
	CP("sgetf22");
    fh = fxp_open_recv(pktin, req);
	CP("sgetf23");

    if (!fh) {
        CP("sgetf29X");
        with_stripctrl(san, fname)
            printf("%s: open for read: %s\n", san, fxp_error());
        return false;
    }

	CP("sgetf30");
	if (outfname!=NULL) // TG
    {
       if (restart) {
		   CP("sgetf33");
		   file = open_existing_wfile(outfname, NULL);
	   } else {
		   CP("sgetf34");
		   file = open_new_file(outfname, GET_PERMISSIONS(attrs, -1));
	   }
	   if (!file) {
		   CP("sgetf35");
		   with_stripctrl(san, outfname)
			   printf("local: unable to open %s\n", san);

		   CP("sgetf36");
		   req = fxp_close_send(fh);
		   CP("sgetf37");
		   pktin = sftp_wait_for_reply(req);
		   CP("sgetf38");
		   fxp_close_recv(pktin, req);

		   CP("sgetf39X");
		   return false;
       }
    }
    else
       file = NULL; // TG: use stream callbacks

    if (restart && file) { // TG
		CP("sgetf40");
		if (seek_file(file, 0, FROM_END) == -1) {
			CP("sgetf41");
			close_wfile(file);
			with_stripctrl(san, outfname)
				printf("reget: cannot restart %s - file too large\n", san);
			CP("sgetf42");
			req = fxp_close_send(fh);
			CP("sgetf43");
			pktin = sftp_wait_for_reply(req);
			CP("sgetf44");
			fxp_close_recv(pktin, req);

			CP("sgetf49X");
			return false;
        }

		CP("sgetf50");
		offset = get_file_posn(file);
		CP("sgetf51");
		printf("reget: restarting at file position %"PRIu64"\n", offset);
	} else {
		CP("sgetf55");
		offset = 0;
	}

	if (outfname) // TG
	{
	   CP("sgetf57");
	   with_stripctrl(san, fname) {
		  with_stripctrl(sano, outfname)
			printf("remote: %s => local:%s\n", san, sano);
	   }
	}
	else
	{
	   CP("sgetf58");
	   printf("remote: %s => stream \n", fname); // TG
	}

	if (curlibctx->timeoutticks<1000) // TG
	   curlibctx->timeoutticks=60000;

	/*
	 * FIXME: we can use FXP_FSTAT here to get the file size, and
	 * thus put up a progress bar.
	 */
	toret = true;
	uint64_t starttick=TGGetTickCount64(); // TG
	uint64_t idlesincetick=0; // TG
	uint64_t TotalBytes=0; // TG
	uint64_t lastprogresstick=starttick; // TG
	bool canceled=false; // TG
	CP("sgetf60");
	xfer = xfer_download_init(fh, offset);
	CP("sgetf61");

	while (!xfer_done(xfer) && !canceled && !curlibctx->aborted) // TG
	{
		CP("sgetf62");
		void *vbuf;
		int retd, len;
		int wpos, wlen;

		uint64_t PrevTotalBytes = TotalBytes; // TG

		//CP("sgetf63");
		xfer_download_queue(xfer);
		//CP("sgetf64");
		pktin = sftp_recv();
		CP("sgetf65");
		retd = xfer_download_gotpkt(xfer, pktin);
		//CP("sgetf66");
		if (retd <= 0) {
			CP("sgetf67");
			if (!shown_err) {
				printf("error while reading: %s\n", fxp_error());
				shown_err = true;
			}
			if (retd == INT_MIN)        /* pktin not even freed */
			{
				CP("sgetf69");
				sfree(pktin);
			}
			toret = false;
		}

		CP("sgetf70");
        bool stopdownload=false;
		while (!stopdownload && xfer_download_data(xfer, &vbuf, &len))
		{
			//CP("sgetf71");
			unsigned char *buf = (unsigned char *)vbuf;

			wpos = 0;
			while (wpos < len)
			{
				//CP("sgetf72");
				if (file) // TG
				{
				   CP("sgetf73");
				   wlen = write_to_file(file, buf + wpos, len - wpos);
				}
				else // TG
				{
				   CP("sgetf74");
				   wlen = curlibctx->write_to_stream(offset, buf + wpos, len - wpos, curlibctx); // TG
				}
				CP("sgetf75");
				if (wlen <= 0)
				{
					CP("sgetf76");
					printf("error while writing local file\n");
					toret = false;
					xfer_set_error(xfer);
					stopdownload=true;
					break;
				}
				wpos += wlen;
				offset += wlen; // TG
			}
			if (wpos < len)
			{          /* we had an error */
				CP("sgetf77");
				toret = false;
				xfer_set_error(xfer);
				stopdownload=true;
			}
			if (!stopdownload)
			{
				TotalBytes+=len; // TG
				//CP("sgetf80");
				if ((curlibctx->progress_callback!=NULL) && // TG
					((TotalBytes % (1024*1024))==0) &&
					(TGGetTickCount64()-lastprogresstick>=1000))
				{
				   CP("sgetf81");
				   if (!curlibctx->progress_callback(TotalBytes,false,curlibctx)) // TG
				   {
					 CP("sgetf82");
					 canceled=true;
					 //eof = true;
					 printf("Canceling ...\n"); // TG
				   }
				   lastprogresstick=TGGetTickCount64(); // TG
				}
			}
			//CP("sgetf83");
			sfree(vbuf);
		}

		if (stopdownload)
		   break;

		CP("sgetf90");
		// check if transfer still going
        if (TotalBytes>PrevTotalBytes) // TG
           idlesincetick = 0; // all good
        else
        {
           if (idlesincetick==0)
              idlesincetick = TGGetTickCount64();
           else
              if (TGGetTickCount64()-idlesincetick > curlibctx->timeoutticks)
              {
		         CP("sgetf95");
				 printf("Timeout error, no more data received.\n"); // TG
                 toret = false;
                 xfer_set_error(xfer);
                 break;
              }
        }
    }
    CP("sgetf96");

	uint64_t endtick=TGGetTickCount64(); // TG
    if (endtick-starttick==0) // TG
       endtick=starttick+1; // prevent divide by zero ;=)

	CP("sgetf97");
	printf("Downloaded %" PRIu64 " Bytes in %" PRIu64 " milliseconds, rate = %" PRIu64 " MB/sec.\n", // TG
		   TotalBytes,
		   (endtick-starttick),
		   (TotalBytes/1024) / (endtick-starttick)
		   );

	CP("sgetf98");
	xfer_cleanup(xfer);

	if (file) // TG
	{
	   CP("sgetf99");
	   close_wfile(file);
	}

    CP("sgetf100");
	req = fxp_close_send(fh);
	CP("sgetf101");
	pktin = sftp_wait_for_reply(req);
	CP("sgetf102");
	fxp_close_recv(pktin, req);

	CP("sgetf103");
	return toret;
}

bool sftp_put_file(char *fname, char *outfname, bool recurse, bool restart)
{
    struct fxp_handle *fh;
    struct fxp_xfer *xfer;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    uint64_t offset;
    RFile *file;
    bool err = false, eof;
    struct fxp_attrs attrs;
    long permissions;

    /*
     * In recursive mode, see if we're dealing with a directory.
     * (If we're not in recursive mode, we need not even check: the
     * subsequent fopen will return an error message.)
     */
    if (recurse && file_type(fname) == FILE_TYPE_DIRECTORY) {
        bool result;
        size_t nnames, namesize;
        char *name, **ournames;
        const char *opendir_err;
        DirHandle *dh;
        size_t i;

        /*
         * First, attempt to create the destination directory,
         * unless it already exists.
         */
        req = fxp_stat_send(outfname);
        pktin = sftp_wait_for_reply(req);
        result = fxp_stat_recv(pktin, req, &attrs);
        if (!result ||
            !(attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) ||
            !(attrs.permissions & 0040000)) {
            req = fxp_mkdir_send(outfname, NULL);
            pktin = sftp_wait_for_reply(req);
            result = fxp_mkdir_recv(pktin, req);

            if (!result) {
                printf("%s: create directory: %s\n",
                       outfname, fxp_error());
                return false;
            }
        }

        /*
         * Now get the list of filenames in the local directory.
         */
        nnames = namesize = 0;
        ournames = NULL;

        dh = open_directory(fname, &opendir_err);
        if (!dh) {
            printf("%s: unable to open directory: %s\n", fname, opendir_err);
            return false;
        }
        while ((name = read_filename(dh)) != NULL) {
            sgrowarray(ournames, namesize, nnames);
            ournames[nnames++] = name;
        }
        close_directory(dh);

        /*
         * Sort the names into a clear order. This ought to make
         * things more predictable when we're doing a reput of the
         * same directory, just in case two readdirs on the same
         * local directory return a different order.
         */
        if (nnames > 0)
            qsort(ournames, nnames, sizeof(*ournames), bare_name_compare);

        /*
         * If we're in restart mode, find the last filename on this
         * list that already exists. We may have to do a reput on
         * _that_ file, but shouldn't have to do anything on the
         * previous files.
         *
         * If none of them exists, of course, we start at 0.
         */
        i = 0;
        if (restart) {
            while (i < nnames) {
                char *nextoutfname;
                nextoutfname = dupcat(outfname, "/", ournames[i]);
                req = fxp_stat_send(nextoutfname);
                pktin = sftp_wait_for_reply(req);
                result = fxp_stat_recv(pktin, req, &attrs);
                sfree(nextoutfname);
                if (!result)
                    break;
                i++;
            }
            if (i > 0)
                i--;
        }

        /*
         * Now we're ready to recurse. Starting at ournames[i]
         * and continuing on to the end of the list, we
         * construct a new source and target file name, and
         * call sftp_put_file again.
         */
        for (; i < nnames; i++) {
            char *nextfname, *nextoutfname;
            bool retd;

            nextfname = dir_file_cat(fname, ournames[i]);
            nextoutfname = dupcat(outfname, "/", ournames[i]);
            retd = sftp_put_file(nextfname, nextoutfname, recurse, restart);
            restart = false;           /* after first partial file, do full */
            sfree(nextoutfname);
            sfree(nextfname);
            if (!retd) {
                for (size_t i = 0; i < nnames; i++) {
                    sfree(ournames[i]);
                }
                sfree(ournames);
                return false;
            }
        }

        /*
         * Done this recursion level. Free everything.
         */
        for (size_t i = 0; i < nnames; i++) {
            sfree(ournames[i]);
        }
        sfree(ournames);

        return true;
    }

    if (fname!=NULL) // TG
    {
       file = open_existing_file(fname, NULL, NULL, NULL, &permissions);
       if (!file) {
           printf("local: unable to open %s\n", fname);
           return false;
       }
    }
    else // TG
    {
       file=NULL; // if null, we will use a callback to read from stream
       permissions=-1; // use default permissions
    }

    attrs.flags = 0;
    PUT_PERMISSIONS(attrs, permissions);
    if (restart) {
        req = fxp_open_send(outfname, SSH_FXF_WRITE, &attrs);
    } else {
        req = fxp_open_send(outfname,
                            SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC,
                            &attrs);
    }
    pktin = sftp_wait_for_reply(req);
    fh = fxp_open_recv(pktin, req);

    if (!fh) {
        if (file) // TG 2019
           close_rfile(file);
        printf("%s: open for write: %s\n", outfname, fxp_error());
        return false;
    }

    if (restart) {
        struct fxp_attrs attrs;
        bool retd;

        req = fxp_fstat_send(fh);
        pktin = sftp_wait_for_reply(req);
        retd = fxp_fstat_recv(pktin, req, &attrs);

        if (!retd) {
            printf("read size of %s: %s\n", outfname, fxp_error());
            err = true;
            goto cleanup;
        }
        if (!(attrs.flags & SSH_FILEXFER_ATTR_SIZE)) {
            printf("read size of %s: size was not given\n", outfname);
            err = true;
            goto cleanup;
        }
        offset = attrs.size;
        printf("reput: restarting at file position %"PRIu64"\n", offset);

        if (file) // TG 2019
           if (seek_file((WFile *)file, offset, FROM_START) != 0)
               seek_file((WFile *)file, 0, FROM_END);    /* *shrug* */
    } else {
        offset = 0;
    }

    if (fname!=NULL) // TG
       printf("local: %s => remote: %s\n", fname, outfname);
    else // TG
       printf("stream => remote: %s\n", outfname); // TG

    /*
     * FIXME: we can use FXP_FSTAT here to get the file size, and
     * thus put up a progress bar.
     */
    xfer = xfer_upload_init(fh, offset);
#ifdef DEBUG_UPLOAD
    printf("calling xfer_upload_init with offset %" PRIu64 "\n",offset); // TG
#endif
	eof = false;
	const int sftpbufsize=16384*4; // TG: much more is not possible, 1MB will definitely fail!
	char *buffer=malloc(sftpbufsize); // TG
	uint64_t starttick=TGGetTickCount64(); // TG
	uint64_t TotalBytes=0; // TG
	uint64_t lastprogresstick=starttick; // TG
    bool canceled=false; // TG
    while (((!err && !eof) || !xfer_done(xfer)) && !canceled && !curlibctx->aborted) // TG
	{
	    // TG: no small buffer on stack!
		int len, ret;

		while (xfer_upload_ready(xfer) && !err && !eof)
		{
            if (file) // TG
			   len = read_from_file(file, buffer, sftpbufsize);
            else
               len = curlibctx->read_from_stream(offset, buffer, sftpbufsize, curlibctx); // TG
			if (len == -1)
			{
				printf("error while reading local file\n");
				err = true;
			}
			else
			  if (len == 0)
			  {
				eof = true;
			  }
			  else
			  {
#ifdef DEBUG_UPLOAD
                printf("calling xfer_upload_data, len is %d\n",len); // TG
#endif
				xfer_upload_data(xfer, buffer, len);
				TotalBytes+=len; // TG
                offset+=len; // TG
			  }
			if ((curlibctx->progress_callback!=NULL) &&   // TG
				((TotalBytes % (1024*1024))==0) &&
				(TGGetTickCount64()-lastprogresstick>=1000))
			{
			   if (!curlibctx->progress_callback(TotalBytes,true,curlibctx))  // TG
			   {
				 canceled=true;
  				 eof = true;
                 printf("Canceling ...\n");
			   }
			   lastprogresstick=TGGetTickCount64();  // TG
			}
        }

        if (toplevel_callback_pending() && !err && !eof) {
            /* If we have pending callbacks, they might make
             * xfer_upload_ready start to return true. So we should
             * run them and then re-check xfer_upload_ready, before
             * we go as far as waiting for an entire packet to
             * arrive. */
            run_toplevel_callbacks();
            continue;
        }

#ifdef DEBUG_UPLOAD
        printf("ensure xfer_done\n"); // TG
#endif
        if (!xfer_done(xfer))
		{
            pktin = sftp_recv();
#ifdef DEBUG_UPLOAD
            printf("calling xfer_upload_gotpkt\n"); // TG
#endif
            ret = xfer_upload_gotpkt(xfer, pktin);
            if (ret <= 0) {
                if (ret == INT_MIN)        /* pktin not even freed */
                    sfree(pktin);
                if (!err) {
                    printf("error while writing: %s\n", fxp_error());
                    err = true;
                }
            }
        }
#ifdef DEBUG_UPLOAD
		else
            printf("xfer_done=true\n"); // TG
#endif
    }
	uint64_t endtick=TGGetTickCount64(); // TG
	free(buffer); // TG

    if (endtick-starttick==0) // TG
       endtick=starttick+1; // prevent divide by zero ;=)
	printf("Uploaded %" PRIu64 " Bytes in %" PRIu64 " milliseconds, rate = %" PRIu64 " MB/sec.\n", // TG
	       TotalBytes,
		   (endtick-starttick),
		   (TotalBytes/1024) / (endtick-starttick)
		   );

#ifdef DEBUG_UPLOAD
    printf("calling xfer_cleanup\n"); // TG
#endif
    xfer_cleanup(xfer);

  cleanup:
    req = fxp_close_send(fh);
    pktin = sftp_wait_for_reply(req);
    if (!fxp_close_recv(pktin, req)) {
        if (!err) {
            printf("error while closing: %s", fxp_error());
            err = true;
        }
    }

    if (file) // TG 2019
    {
       close_rfile(file);
    }

    return !err;
}

/* ----------------------------------------------------------------------
 * A remote wildcard matcher, providing a similar interface to the
 * local one in psftp.h.
 */

typedef struct SftpWildcardMatcher {
    struct fxp_handle *dirh;
    struct fxp_names *names;
    int namepos;
    char *wildcard, *prefix;
} SftpWildcardMatcher;

SftpWildcardMatcher *sftp_begin_wildcard_matching(char *name)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    char *wildcard;
    char *unwcdir, *tmpdir, *cdir;
    int len;
    bool check;
    SftpWildcardMatcher *swcm;
    struct fxp_handle *dirh;

    /*
     * We don't handle multi-level wildcards; so we expect to find
     * a fully specified directory part, followed by a wildcard
     * after that.
     */
    wildcard = stripslashes(name, false);

    unwcdir = dupstr(name);
    len = (int) (wildcard - name);   // TG
    unwcdir[len] = '\0';
    if (len > 0 && unwcdir[len-1] == '/')
        unwcdir[len-1] = '\0';
    tmpdir = snewn(1 + len, char);
    check = wc_unescape(tmpdir, unwcdir);
    sfree(tmpdir);

    if (!check) {
        printf("Multiple-level wildcards are not supported\n");
        sfree(unwcdir);
        return NULL;
    }

    cdir = canonify(unwcdir);

    req = fxp_opendir_send(cdir);
    pktin = sftp_wait_for_reply(req);
    dirh = fxp_opendir_recv(pktin, req);

    if (dirh) {
        swcm = snew(SftpWildcardMatcher);
        swcm->dirh = dirh;
        swcm->names = NULL;
        swcm->wildcard = dupstr(wildcard);
        swcm->prefix = unwcdir;
    } else {
        printf("Unable to open %s: %s\n", cdir, fxp_error());
        swcm = NULL;
        sfree(unwcdir);
    }

    sfree(cdir);

    return swcm;
}

char *sftp_wildcard_get_filename(SftpWildcardMatcher *swcm)
{
    struct fxp_name *name;
    struct sftp_packet *pktin;
    struct sftp_request *req;

    while (1) {
        if (swcm->names && swcm->namepos >= swcm->names->nnames) {
            fxp_free_names(swcm->names);
            swcm->names = NULL;
        }

        if (!swcm->names) {
            req = fxp_readdir_send(swcm->dirh);
            pktin = sftp_wait_for_reply(req);
            swcm->names = fxp_readdir_recv(pktin, req);

            if (!swcm->names) {
                if (fxp_error_type() != SSH_FX_EOF) {
                    with_stripctrl(san, swcm->prefix)
                        printf("%s: reading directory: %s\n",
                               san, fxp_error());
                }
                return NULL;
            } else if (swcm->names->nnames == 0) {
                /*
                 * Another failure mode which we treat as EOF is if
                 * the server reports success from FXP_READDIR but
                 * returns no actual names. This is unusual, since
                 * from most servers you'd expect at least "." and
                 * "..", but there's nothing forbidding a server from
                 * omitting those if it wants to.
                 */
                return NULL;
            }

            swcm->namepos = 0;
        }

        assert(swcm->names && swcm->namepos < swcm->names->nnames);

        name = &swcm->names->names[swcm->namepos++];

        if (!strcmp(name->filename, ".") || !strcmp(name->filename, ".."))
            continue;                  /* expected bad filenames */

        if (!vet_filename(name->filename)) {
            with_stripctrl(san, name->filename)
                printf("ignoring potentially dangerous server-"
                       "supplied filename '%s'\n", san);
            continue;                  /* unexpected bad filename */
        }

        if (!wc_match(swcm->wildcard, name->filename))
            continue;                  /* doesn't match the wildcard */

        /*
         * We have a working filename. Return it.
         */
        return dupprintf("%s%s%s", swcm->prefix,
                         (!swcm->prefix[0] ||
                          swcm->prefix[strlen(swcm->prefix)-1]=='/' ?
                          "" : "/"),
                         name->filename);
    }
}

void sftp_finish_wildcard_matching(SftpWildcardMatcher *swcm)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    req = fxp_close_send(swcm->dirh);
    pktin = sftp_wait_for_reply(req);
    fxp_close_recv(pktin, req);

    if (swcm->names)
        fxp_free_names(swcm->names);

    sfree(swcm->prefix);
    sfree(swcm->wildcard);

    sfree(swcm);
}

/*
 * General function to match a potential wildcard in a filename
 * argument and iterate over every matching file. Used in several
 * PSFTP commands (rmdir, rm, chmod, mv).
 */
bool wildcard_iterate(char *filename, bool (*func)(void *, char *), void *ctx)
{
	char *unwcfname,*cname; // TG
    bool toret; // TG

#ifndef TGDLL
    char *newname, // TG
	bool is_wc; // TG
#endif

	unwcfname = snewn(strlen(filename)+1, char);

#ifdef TGDLL
	// no wildcard support in DLL!
	// causes big problems, for example filenames with brackets are considered
	// to contain wildcards
	strcpy(unwcfname,filename);
#else
	is_wc = !wc_unescape(unwcfname, filename);

    if (is_wc) {
        SftpWildcardMatcher *swcm = sftp_begin_wildcard_matching(filename);
        bool matched = false;
        sfree(unwcfname);

        if (!swcm)
            return false;

        toret = true;

        while ( (newname = sftp_wildcard_get_filename(swcm)) != NULL ) {
            cname = canonify(newname);
            sfree(newname);
            matched = true;
            if (!func(ctx, cname))
                toret = false;
            sfree(cname);
        }

        if (!matched) {
            /* Politely warn the user that nothing matched. */
            printf("%s: nothing matched\n", filename);
        }

        sftp_finish_wildcard_matching(swcm);
	}
	else
#endif
	{
		cname = canonify(unwcfname);
		toret = func(ctx, cname);
		sfree(cname);
		sfree(unwcfname);
	}

	return toret;
}

/*
 * Handy helper function.
 */
bool is_wildcard(char *name)
{
#ifdef TGDLL
	return false; // no wildcard support
#else
	char *unwcfname = snewn(strlen(name)+1, char);
	bool is_wc = !wc_unescape(unwcfname, name);
	sfree(unwcfname);
	return is_wc;
#endif
}

/* ----------------------------------------------------------------------
 * Actual sftp commands.
 */
struct sftp_command {
    char **words;
    size_t nwords, wordssize;
    int (*obey) (struct sftp_command *);        /* returns <0 to quit */
};

int sftp_cmd_null(struct sftp_command *cmd)
{
    return 1;                          /* success */
}

int sftp_cmd_unknown(struct sftp_command *cmd)
{
    printf("psftp: unknown command \"%s\"\n", cmd->words[0]);
    return 0;                          /* failure */
}

int sftp_cmd_quit(struct sftp_command *cmd)
{
    return -1;
}

int sftp_cmd_close(struct sftp_command *cmd)
{
    if (!backend) {
        not_connected();
        return 0;
    }

    if (backend_connected(backend)) {
        char ch;
        backend_special(backend, SS_EOF, 0);
        sent_eof = true;
        sftp_recvdata(&ch, 1);
    }
    do_sftp_cleanup();

    return 0;
}

void list_directory_from_sftp_warn_unsorted(void)
{
    printf("Directory is too large to sort; writing file names unsorted\n");
}

void list_directory_from_sftp_print(struct fxp_name *name)
{
    with_stripctrl(san, name->longname)
        printf("%s\n", san);
}

/*
 * List a directory. If no arguments are given, list pwd; otherwise
 * list the directory given in words[1].
 */
int sftp_cmd_ls(struct sftp_command *cmd)
{
    struct fxp_handle *dirh;
    struct fxp_names *names;
    const char *dir;
    char *cdir, *unwcdir, *wildcard;
    struct sftp_packet *pktin;
    struct sftp_request *req;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2)
        dir = ".";
    else
        dir = cmd->words[1];

    unwcdir = snewn(1 + strlen(dir), char);
    if (wc_unescape(unwcdir, dir)) {
        dir = unwcdir;
        wildcard = NULL;
    } else {
        char *tmpdir;
        int len;
        bool check;

        sfree(unwcdir);
        wildcard = stripslashes(dir, false);
        unwcdir = dupstr(dir);
        len = (int) (wildcard - dir); // TG
        unwcdir[len] = '\0';
        if (len > 0 && unwcdir[len-1] == '/')
            unwcdir[len-1] = '\0';
        tmpdir = snewn(1 + len, char);
        check = wc_unescape(tmpdir, unwcdir);
        sfree(tmpdir);
        if (!check) {
            printf("Multiple-level wildcards are not supported\n");
            sfree(unwcdir);
            return 0;
        }
        dir = unwcdir;
    }

    cdir = canonify(dir);

    #ifndef TGDLL
    with_stripctrl(san, cdir)
        printf("Listing directory %s\n", san);
    #endif

    req = fxp_opendir_send(cdir);
    pktin = sftp_wait_for_reply(req);
    dirh = fxp_opendir_recv(pktin, req);

    if (dirh == NULL) {
        printf("Unable to open %s: %s\n", dir, fxp_error());
        sfree(cdir);
        sfree(unwcdir);
        return 0;
    } else {
        struct list_directory_from_sftp_ctx *ctx =
		  TGDLLCODE(curlibctx && curlibctx->ls_callback ? 0 : )
            list_directory_from_sftp_new();

        while (1) {

            req = fxp_readdir_send(dirh);
            pktin = sftp_wait_for_reply(req);
            names = fxp_readdir_recv(pktin, req);

            if (names == NULL) {
                if (fxp_error_type() == SSH_FX_EOF)
                    break;
                printf("Reading directory %s: %s\n", dir, fxp_error());
                break;
            }
            if (names->nnames == 0) {
                fxp_free_names(names);
                break;
            }

            if (!ctx)
               curlibctx->ls_callback(names,curlibctx); // TG
            else
            for (size_t i = 0; i < names->nnames; i++)
                if (!wildcard || wc_match(wildcard, names->names[i].filename))
                    list_directory_from_sftp_feed(ctx, &names->names[i]);

            fxp_free_names(names);
        }

        req = fxp_close_send(dirh);
        pktin = sftp_wait_for_reply(req);
        fxp_close_recv(pktin, req);

		if (ctx) // TG
		{
		   list_directory_from_sftp_finish(ctx);
		   list_directory_from_sftp_free(ctx);
		}
	}

    sfree(cdir);
    sfree(unwcdir);

    return 1;
}

/*
 * Change directories. We do this by canonifying the new name, then
 * trying to OPENDIR it. Only if that succeeds do we set the new pwd.
 */
int sftp_cmd_cd(struct sftp_command *cmd)
{
    struct fxp_handle *dirh;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    char *dir;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2)
        dir = dupstr(homedir);
    else {
        dir = canonify(cmd->words[1]);
    }

    req = fxp_opendir_send(dir);
    pktin = sftp_wait_for_reply(req);
    dirh = fxp_opendir_recv(pktin, req);

    if (!dirh) {
        with_stripctrl(san, dir)
            printf("Directory %s: %s\n", san, fxp_error());
        sfree(dir);
        return 0;
    }

    req = fxp_close_send(dirh);
    pktin = sftp_wait_for_reply(req);
    fxp_close_recv(pktin, req);

    sfree(pwd);
    pwd = dir;
    #ifndef TGDLL
    with_stripctrl(san, pwd)
        printf("Remote directory is now %s\n", san);
    #endif

    return 1;
}

/*
 * Print current directory. Easy as pie.
 */
int sftp_cmd_pwd(struct sftp_command *cmd)
{
    if (!backend) {
        not_connected();
        return 0;
    }

    with_stripctrl(san, pwd)
        printf("Remote directory is %s\n", san);
    return 1;
}

/*
 * Get a file and save it at the local end. We have three very
 * similar commands here. The basic one is `get'; `reget' differs
 * in that it checks for the existence of the destination file and
 * starts from where a previous aborted transfer left off; `mget'
 * differs in that it interprets all its arguments as files to
 * transfer (never as a different local name for a remote file) and
 * can handle wildcards.
 */
int sftp_general_get(struct sftp_command *cmd, bool restart, bool multiple)
{
    char *fname, *unwcfname, *origfname, *origwfname, *outfname;
    int i, toret;
    bool recurse = false;

    if (!backend) {
        not_connected();
        return 0;
    }

    i = 1;
    while (i < cmd->nwords && cmd->words[i][0] == '-') {
        if (!strcmp(cmd->words[i], "--")) {
            /* finish processing options */
            i++;
            break;
        } else if (!strcmp(cmd->words[i], "-r")) {
            recurse = true;
        } else {
            printf("%s: unrecognised option '%s'\n", cmd->words[0], cmd->words[i]);
            return 0;
        }
        i++;
    }

    if (i >= cmd->nwords) {
        printf("%s: expects a filename\n", cmd->words[0]);
        return 0;
    }

    toret = 1;
    do {
        SftpWildcardMatcher *swcm;

        origfname = cmd->words[i++];
        unwcfname = snewn(strlen(origfname)+1, char);

        if (multiple && !wc_unescape(unwcfname, origfname)) {
            swcm = sftp_begin_wildcard_matching(origfname);
            if (!swcm) {
                sfree(unwcfname);
                continue;
            }
            origwfname = sftp_wildcard_get_filename(swcm);
            if (!origwfname) {
                /* Politely warn the user that nothing matched. */
                printf("%s: nothing matched\n", origfname);
                sftp_finish_wildcard_matching(swcm);
                sfree(unwcfname);
                continue;
            }
        } else {
            origwfname = origfname;
            swcm = NULL;
        }

        while (origwfname) {
            fname = canonify(origwfname);

            if (!multiple && i < cmd->nwords)
                outfname = cmd->words[i++];
            else
                outfname = stripslashes(origwfname, false);

            toret = sftp_get_file(fname, outfname, recurse, restart);

            sfree(fname);

            if (swcm) {
                sfree(origwfname);
                origwfname = sftp_wildcard_get_filename(swcm);
            } else {
                origwfname = NULL;
            }
        }
        sfree(unwcfname);
        if (swcm)
            sftp_finish_wildcard_matching(swcm);
        if (!toret)
            return toret;

    } while (multiple && i < cmd->nwords);

    return toret;
}
int sftp_cmd_get(struct sftp_command *cmd)
{
    return sftp_general_get(cmd, false, false);
}
int sftp_cmd_mget(struct sftp_command *cmd)
{
    return sftp_general_get(cmd, false, true);
}
int sftp_cmd_reget(struct sftp_command *cmd)
{
    return sftp_general_get(cmd, true, false);
}

/*
 * Send a file and store it at the remote end. We have three very
 * similar commands here. The basic one is `put'; `reput' differs
 * in that it checks for the existence of the destination file and
 * starts from where a previous aborted transfer left off; `mput'
 * differs in that it interprets all its arguments as files to
 * transfer (never as a different remote name for a local file) and
 * can handle wildcards.
 */
int sftp_general_put(struct sftp_command *cmd, bool restart, bool multiple)
{
    char *fname, *wfname, *origoutfname, *outfname;
    int i;
    int toret;
    bool recurse = false;

    if (!backend) {
        not_connected();
        return 0;
    }

    i = 1;
    while (i < cmd->nwords && cmd->words[i][0] == '-') {
        if (!strcmp(cmd->words[i], "--")) {
            /* finish processing options */
            i++;
            break;
        } else if (!strcmp(cmd->words[i], "-r")) {
            recurse = true;
        } else {
            printf("%s: unrecognised option '%s'\n", cmd->words[0], cmd->words[i]);
            return 0;
        }
        i++;
    }

    if (i >= cmd->nwords) {
        printf("%s: expects a filename\n", cmd->words[0]);
        return 0;
    }

    toret = 1;
    do {
        WildcardMatcher *wcm;
        fname = cmd->words[i++];

        if (multiple && test_wildcard(fname, false) == WCTYPE_WILDCARD) {
            wcm = begin_wildcard_matching(fname);
            wfname = wildcard_get_filename(wcm);
            if (!wfname) {
                /* Politely warn the user that nothing matched. */
                printf("%s: nothing matched\n", fname);
                finish_wildcard_matching(wcm);
                continue;
            }
        } else {
            wfname = fname;
            wcm = NULL;
        }

        while (wfname) {
            if (!multiple && i < cmd->nwords)
                origoutfname = cmd->words[i++];
            else
                origoutfname = stripslashes(wfname, true);

            outfname = canonify(origoutfname);
            toret = sftp_put_file(wfname, outfname, recurse, restart);
            sfree(outfname);

            if (wcm) {
                sfree(wfname);
                wfname = wildcard_get_filename(wcm);
            } else {
                wfname = NULL;
            }
        }

        if (wcm)
            finish_wildcard_matching(wcm);

        if (!toret)
            return toret;

    } while (multiple && i < cmd->nwords);

    return toret;
}
int sftp_cmd_put(struct sftp_command *cmd)
{
    return sftp_general_put(cmd, false, false);
}
int sftp_cmd_mput(struct sftp_command *cmd)
{
    return sftp_general_put(cmd, false, true);
}
int sftp_cmd_reput(struct sftp_command *cmd)
{
    return sftp_general_put(cmd, true, false);
}

int sftp_cmd_mkdir(struct sftp_command *cmd)
{
    char *dir;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    bool result;
    int i, ret;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2) {
        printf("mkdir: expects a directory\n");
        return 0;
    }

    ret = 1;
    for (i = 1; i < cmd->nwords; i++) {
        dir = canonify(cmd->words[i]);

        req = fxp_mkdir_send(dir, NULL);
        pktin = sftp_wait_for_reply(req);
        result = fxp_mkdir_recv(pktin, req);

        if (!result) {
            with_stripctrl(san, dir)
                printf("mkdir %s: %s\n", san, fxp_error());
            ret = 0;
        } else
            with_stripctrl(san, dir)
                printf("mkdir %s: OK\n", san);

        sfree(dir);
    }

    return ret;
}

static bool sftp_action_rmdir(void *vctx, char *dir)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    bool result;

    req = fxp_rmdir_send(dir);
    pktin = sftp_wait_for_reply(req);
    result = fxp_rmdir_recv(pktin, req);

    if (!result) {
        printf("rmdir %s: %s\n", dir, fxp_error());
        return false;
    }

    printf("rmdir %s: OK\n", dir);

    return true;
}

int sftp_cmd_rmdir(struct sftp_command *cmd)
{
    int i, ret;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2) {
        printf("rmdir: expects a directory\n");
        return 0;
    }

    ret = 1;
    for (i = 1; i < cmd->nwords; i++)
        ret &= wildcard_iterate(cmd->words[i], sftp_action_rmdir, NULL);

    return ret;
}

static bool sftp_action_rm(void *vctx, char *fname)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    bool result;

    req = fxp_remove_send(fname);
    pktin = sftp_wait_for_reply(req);
    result = fxp_remove_recv(pktin, req);

    if (!result) {
        printf("rm %s: %s\n", fname, fxp_error());
        return false;
    }

    printf("rm %s: OK\n", fname);

    return true;
}

int sftp_cmd_rm(struct sftp_command *cmd)
{
    int i, ret;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 2) {
        printf("rm: expects a filename\n");
        return 0;
    }

    ret = 1;
    for (i = 1; i < cmd->nwords; i++)
        ret &= wildcard_iterate(cmd->words[i], sftp_action_rm, NULL);

    return ret;
}

static bool check_is_dir(char *dstfname)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;
    struct fxp_attrs attrs;
    bool result;

    req = fxp_stat_send(dstfname);
    pktin = sftp_wait_for_reply(req);
    result = fxp_stat_recv(pktin, req, &attrs);

    if (result &&
        (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) &&
        (attrs.permissions & 0040000))
        return true;
    else
        return false;
}

static bool get_stat(char *dstfname, struct fxp_attrs *attrs) // TG
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    bool result;

    req = fxp_stat_send(dstfname);
    pktin = sftp_wait_for_reply(req);
    result = fxp_stat_recv(pktin, req, attrs);

    return result;
}

static bool set_stat(char *dstfname, struct fxp_attrs *attrs) // TG
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    bool result;

    req = fxp_setstat_send(dstfname, *attrs);
    pktin = sftp_wait_for_reply(req);
    result = fxp_setstat_recv(pktin, req);

    return result;
}

struct sftp_context_mv {
    char *dstfname;
    bool dest_is_dir;
};

static bool sftp_action_mv(void *vctx, char *srcfname)
{
    struct sftp_context_mv *ctx = (struct sftp_context_mv *)vctx;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    const char *error;
    char *finalfname, *newcanon = NULL;
    bool toret, result;

    if (ctx->dest_is_dir) {
        char *p;
        char *newname;

        p = srcfname + strlen(srcfname);
        while (p > srcfname && p[-1] != '/') p--;
        newname = dupcat(ctx->dstfname, "/", p);
        newcanon = canonify(newname);
        sfree(newname);

        finalfname = newcanon;
    } else {
        finalfname = ctx->dstfname;
    }

    printf("Renaming %s to %s\n",srcfname, finalfname); // TG
    req = fxp_rename_send(srcfname, finalfname);
    pktin = sftp_wait_for_reply(req);
    result = fxp_rename_recv(pktin, req);

    error = result ? NULL : fxp_error();

    if (error) {
        with_stripctrl(san, finalfname)
            printf("mv %s %s: %s\n", srcfname, san, error);
        toret = false;
    } else {
        with_stripctrl(san, finalfname)
            printf("%s -> %s\n", srcfname, san);
        toret = true;
    }

    sfree(newcanon);
    return toret;
}

int sftp_cmd_mvex(struct sftp_command *cmd,const int moveflags) // TG 2019
{
    struct sftp_context_mv ctx[1];
    int i, ret;

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 3) {
        printf("mv: expects two filenames\n");
        return 0;
    }

    ctx->dstfname = canonify(cmd->words[cmd->nwords-1]);

    if ((moveflags & cMoveFlag_DestinationPathIncludesItemName) != 0) // TG
       ctx->dest_is_dir = false;
    else
       if ((moveflags & cMoveFlag_AddSourceItemNameToDestinationPath) != 0) // TG
          ctx->dest_is_dir = true;
       else
          ctx->dest_is_dir = check_is_dir(ctx->dstfname); // TG

	#ifndef TGDLL
    // BIG PROBLEM! is_wildcard could return true if filename contains brackets
    // this check is not needed for a DLL
    /*
     * If there's more than one source argument, or one source
     * argument which is a wildcard, we _require_ that the
     * destination is a directory.
     */
    ctx->dest_is_dir = check_is_dir(ctx->dstfname);
    if ((cmd->nwords > 3 || is_wildcard(cmd->words[1])) && !ctx->dest_is_dir) {
        printf("mv: multiple or wildcard arguments require the destination"
               " to be a directory\n");
        sfree(ctx->dstfname);
        return 0;
    }
	#endif

    /*
     * Now iterate over the source arguments.
     */
    ret = 1;
    for (i = 1; i < cmd->nwords-1; i++)
        ret &= wildcard_iterate(cmd->words[i], sftp_action_mv, ctx);

    sfree(ctx->dstfname);
    return ret;
}

int sftp_cmd_mv(struct sftp_command *cmd) // TG 2019
{
  return sftp_cmd_mvex(cmd,0);
}

struct sftp_context_chmod {
    unsigned attrs_clr, attrs_xor;
};

static bool sftp_action_chmod(void *vctx, char *fname)
{
    struct fxp_attrs attrs;
    struct sftp_packet *pktin;
    struct sftp_request *req;
    bool result;
    unsigned oldperms, newperms;
    struct sftp_context_chmod *ctx = (struct sftp_context_chmod *)vctx;

    req = fxp_stat_send(fname);
    pktin = sftp_wait_for_reply(req);
    result = fxp_stat_recv(pktin, req, &attrs);

    if (!result || !(attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS)) {
        printf("get attrs for %s: %s\n", fname,
               result ? "file permissions not provided" : fxp_error());
        return false;
    }

    attrs.flags = SSH_FILEXFER_ATTR_PERMISSIONS;   /* perms _only_ */
    oldperms = attrs.permissions & 07777;
    attrs.permissions &= ~ctx->attrs_clr;
    attrs.permissions ^= ctx->attrs_xor;
    newperms = attrs.permissions & 07777;

    if (oldperms == newperms)
        return true;                   /* no need to do anything! */

    req = fxp_setstat_send(fname, attrs);
    pktin = sftp_wait_for_reply(req);
    result = fxp_setstat_recv(pktin, req);

    if (!result) {
        printf("set attrs for %s: %s\n", fname, fxp_error());
        return false;
    }

    printf("%s: %04o -> %04o\n", fname, oldperms, newperms);

    return true;
}

int sftp_cmd_chmod(struct sftp_command *cmd)
{
    char *mode;
    int i, ret;
    struct sftp_context_chmod ctx[1];

    if (!backend) {
        not_connected();
        return 0;
    }

    if (cmd->nwords < 3) {
        printf("chmod: expects a mode specifier and a filename\n");
        return 0;
    }

    /*
     * Attempt to parse the mode specifier in cmd->words[1]. We
     * don't support the full horror of Unix chmod; instead we
     * support a much simpler syntax in which the user can either
     * specify an octal number, or a comma-separated sequence of
     * [ugoa]*[-+=][rwxst]+. (The initial [ugoa] sequence may
     * _only_ be omitted if the only attribute mentioned is t,
     * since all others require a user/group/other specification.
     * Additionally, the s attribute may not be specified for any
     * [ugoa] specifications other than exactly u or exactly g.
     */
    ctx->attrs_clr = ctx->attrs_xor = 0;
    mode = cmd->words[1];
    if (mode[0] >= '0' && mode[0] <= '9') {
        if (mode[strspn(mode, "01234567")]) {
            printf("chmod: numeric file modes should"
                   " contain digits 0-7 only\n");
            return 0;
        }
        ctx->attrs_clr = 07777;
        sscanf(mode, "%o", &ctx->attrs_xor);
        ctx->attrs_xor &= ctx->attrs_clr;
    } else {
        while (*mode) {
            char *modebegin = mode;
            unsigned subset, perms;
            int action;

            subset = 0;
            while (*mode && *mode != ',' &&
                   *mode != '+' && *mode != '-' && *mode != '=') {
                switch (*mode) {
                  case 'u': subset |= 04700; break; /* setuid, user perms */
                  case 'g': subset |= 02070; break; /* setgid, group perms */
                  case 'o': subset |= 00007; break; /* just other perms */
                  case 'a': subset |= 06777; break; /* all of the above */
                  default:
                    printf("chmod: file mode '%.*s' contains unrecognised"
                           " user/group/other specifier '%c'\n",
                           (int)strcspn(modebegin, ","), modebegin, *mode);
                    return 0;
                }
                mode++;
            }
            if (!*mode || *mode == ',') {
                printf("chmod: file mode '%.*s' is incomplete\n",
                       (int)strcspn(modebegin, ","), modebegin);
                return 0;
            }
            action = *mode++;
            if (!*mode || *mode == ',') {
                printf("chmod: file mode '%.*s' is incomplete\n",
                       (int)strcspn(modebegin, ","), modebegin);
                return 0;
            }
            perms = 0;
            while (*mode && *mode != ',') {
                switch (*mode) {
                  case 'r': perms |= 00444; break;
                  case 'w': perms |= 00222; break;
                  case 'x': perms |= 00111; break;
                  case 't': perms |= 01000; subset |= 01000; break;
                  case 's':
                    if ((subset & 06777) != 04700 &&
                        (subset & 06777) != 02070) {
                        printf("chmod: file mode '%.*s': set[ug]id bit should"
                               " be used with exactly one of u or g only\n",
                               (int)strcspn(modebegin, ","), modebegin);
                        return 0;
                    }
                    perms |= 06000;
                    break;
                  default:
                    printf("chmod: file mode '%.*s' contains unrecognised"
                           " permission specifier '%c'\n",
                           (int)strcspn(modebegin, ","), modebegin, *mode);
                    return 0;
                }
                mode++;
            }
            if (!(subset & 06777) && (perms &~ subset)) {
                printf("chmod: file mode '%.*s' contains no user/group/other"
                       " specifier and permissions other than 't' \n",
                       (int)strcspn(modebegin, ","), modebegin);
                return 0;
            }
            perms &= subset;
            switch (action) {
              case '+':
                ctx->attrs_clr |= perms;
                ctx->attrs_xor |= perms;
                break;
              case '-':
                ctx->attrs_clr |= perms;
                ctx->attrs_xor &= ~perms;
                break;
              case '=':
                ctx->attrs_clr |= subset;
                ctx->attrs_xor |= perms;
                break;
            }
            if (*mode) mode++;         /* eat comma */
        }
    }

    ret = 1;
    for (i = 2; i < cmd->nwords; i++)
        ret &= wildcard_iterate(cmd->words[i], sftp_action_chmod, ctx);

    return ret;
}

static int sftp_cmd_open(struct sftp_command *cmd)
{
    int portnumber;

    if (backend) {
        printf("psftp: already connected\n");
        return 0;
    }

    if (cmd->nwords < 2) {
        printf("open: expects a host name\n");
        return 0;
    }

    if (cmd->nwords > 2) {
        portnumber = atoi(cmd->words[2]);
        if (portnumber == 0) {
            printf("open: invalid port number\n");
            return 0;
        }
    } else
        portnumber = 0;

    if (psftp_connect(cmd->words[1], NULL, portnumber)) {
        backend = NULL;                /* connection is already closed */
        return -1;                     /* this is fatal */
    }
    do_sftp_init();
    return 1;
}

static int sftp_cmd_lcd(struct sftp_command *cmd)
{
    char *currdir, *errmsg;

    if (cmd->nwords < 2) {
        printf("lcd: expects a local directory name\n");
        return 0;
    }

    errmsg = psftp_lcd(cmd->words[1]);
    if (errmsg) {
        printf("lcd: unable to change directory: %s\n", errmsg);
        sfree(errmsg);
        return 0;
    }

    currdir = psftp_getcwd();
    printf("New local directory is %s\n", currdir);
    sfree(currdir);

    return 1;
}

static int sftp_cmd_lpwd(struct sftp_command *cmd)
{
    char *currdir;

    currdir = psftp_getcwd();
    printf("Current local directory is %s\n", currdir);
    sfree(currdir);

    return 1;
}

static int sftp_cmd_pling(struct sftp_command *cmd)
{
    int exitcode;

    exitcode = system(cmd->words[1]);
    return (exitcode == 0);
}

static int sftp_cmd_help(struct sftp_command *cmd);

static struct sftp_cmd_lookup {
    const char *name;
    /*
     * For help purposes, there are two kinds of command:
     *
     *  - primary commands, in which `longhelp' is non-NULL. In
     *    this case `shorthelp' is descriptive text, and `longhelp'
     *    is longer descriptive text intended to be printed after
     *    the command name.
     *
     *  - alias commands, in which `longhelp' is NULL. In this case
     *    `shorthelp' is the name of a primary command, which
     *    contains the help that should double up for this command.
     */
    bool listed;                /* do we list this in primary help? */
    const char *shorthelp;
    const char *longhelp;
    int (*obey) (struct sftp_command *);
} sftp_lookup[] = {
    /*
     * List of sftp commands. This is binary-searched so it MUST be
     * in ASCII order.
     */
    {
        "!", true, "run a local command",
            "<command>\n"
            /* FIXME: this example is crap for non-Windows. */
            "  Runs a local command. For example, \"!del myfile\".\n",
            sftp_cmd_pling
    },
    {
        "bye", true, "finish your SFTP session",
            "\n"
            "  Terminates your SFTP session and quits the PSFTP program.\n",
            sftp_cmd_quit
    },
    {
        "cd", true, "change your remote working directory",
            " [ <new working directory> ]\n"
            "  Change the remote working directory for your SFTP session.\n"
            "  If a new working directory is not supplied, you will be\n"
            "  returned to your home directory.\n",
            sftp_cmd_cd
    },
    {
        "chmod", true, "change file permissions and modes",
            " <modes> <filename-or-wildcard> [ <filename-or-wildcard>... ]\n"
            "  Change the file permissions on one or more remote files or\n"
            "  directories.\n"
            "  <modes> can be any octal Unix permission specifier.\n"
            "  Alternatively, <modes> can include the following modifiers:\n"
            "    u+r     make file readable by owning user\n"
            "    u+w     make file writable by owning user\n"
            "    u+x     make file executable by owning user\n"
            "    u-r     make file not readable by owning user\n"
            "    [also u-w, u-x]\n"
            "    g+r     make file readable by members of owning group\n"
            "    [also g+w, g+x, g-r, g-w, g-x]\n"
            "    o+r     make file readable by all other users\n"
            "    [also o+w, o+x, o-r, o-w, o-x]\n"
            "    a+r     make file readable by absolutely everybody\n"
            "    [also a+w, a+x, a-r, a-w, a-x]\n"
            "    u+s     enable the Unix set-user-ID bit\n"
            "    u-s     disable the Unix set-user-ID bit\n"
            "    g+s     enable the Unix set-group-ID bit\n"
            "    g-s     disable the Unix set-group-ID bit\n"
            "    +t      enable the Unix \"sticky bit\"\n"
            "  You can give more than one modifier for the same user (\"g-rwx\"), and\n"
            "  more than one user for the same modifier (\"ug+w\"). You can\n"
            "  use commas to separate different modifiers (\"u+rwx,g+s\").\n",
            sftp_cmd_chmod
    },
    {
        "close", true, "finish your SFTP session but do not quit PSFTP",
            "\n"
            "  Terminates your SFTP session, but does not quit the PSFTP\n"
            "  program. You can then use \"open\" to start another SFTP\n"
            "  session, to the same server or to a different one.\n",
            sftp_cmd_close
    },
    {
        "del", true, "delete files on the remote server",
            " <filename-or-wildcard> [ <filename-or-wildcard>... ]\n"
            "  Delete a file or files from the server.\n",
            sftp_cmd_rm
    },
    {
        "delete", false, "del", NULL, sftp_cmd_rm
    },
    {
        "dir", true, "list remote files",
            " [ <directory-name> ]/[ <wildcard> ]\n"
            "  List the contents of a specified directory on the server.\n"
            "  If <directory-name> is not given, the current working directory\n"
            "  is assumed.\n"
            "  If <wildcard> is given, it is treated as a set of files to\n"
            "  list; otherwise, all files are listed.\n",
            sftp_cmd_ls
    },
    {
        "exit", true, "bye", NULL, sftp_cmd_quit
    },
    {
        "get", true, "download a file from the server to your local machine",
            " [ -r ] [ -- ] <filename> [ <local-filename> ]\n"
            "  Downloads a file on the server and stores it locally under\n"
            "  the same name, or under a different one if you supply the\n"
            "  argument <local-filename>.\n"
            "  If -r specified, recursively fetch a directory.\n",
            sftp_cmd_get
    },
    {
        "help", true, "give help",
            " [ <command> [ <command> ... ] ]\n"
            "  Give general help if no commands are specified.\n"
            "  If one or more commands are specified, give specific help on\n"
            "  those particular commands.\n",
            sftp_cmd_help
    },
    {
        "lcd", true, "change local working directory",
            " <local-directory-name>\n"
            "  Change the local working directory of the PSFTP program (the\n"
            "  default location where the \"get\" command will save files).\n",
            sftp_cmd_lcd
    },
    {
        "lpwd", true, "print local working directory",
            "\n"
            "  Print the local working directory of the PSFTP program (the\n"
            "  default location where the \"get\" command will save files).\n",
            sftp_cmd_lpwd
    },
    {
        "ls", true, "dir", NULL,
            sftp_cmd_ls
    },
    {
        "mget", true, "download multiple files at once",
            " [ -r ] [ -- ] <filename-or-wildcard> [ <filename-or-wildcard>... ]\n"
            "  Downloads many files from the server, storing each one under\n"
            "  the same name it has on the server side. You can use wildcards\n"
            "  such as \"*.c\" to specify lots of files at once.\n"
            "  If -r specified, recursively fetch files and directories.\n",
            sftp_cmd_mget
    },
    {
        "mkdir", true, "create directories on the remote server",
            " <directory-name> [ <directory-name>... ]\n"
            "  Creates directories with the given names on the server.\n",
            sftp_cmd_mkdir
    },
    {
        "mput", true, "upload multiple files at once",
            " [ -r ] [ -- ] <filename-or-wildcard> [ <filename-or-wildcard>... ]\n"
            "  Uploads many files to the server, storing each one under the\n"
            "  same name it has on the client side. You can use wildcards\n"
            "  such as \"*.c\" to specify lots of files at once.\n"
            "  If -r specified, recursively store files and directories.\n",
            sftp_cmd_mput
    },
    {
        "mv", true, "move or rename file(s) on the remote server",
            " <source> [ <source>... ] <destination>\n"
            "  Moves or renames <source>(s) on the server to <destination>,\n"
            "  also on the server.\n"
            "  If <destination> specifies an existing directory, then <source>\n"
            "  may be a wildcard, and multiple <source>s may be given; all\n"
            "  source files are moved into <destination>.\n"
            "  Otherwise, <source> must specify a single file, which is moved\n"
            "  or renamed so that it is accessible under the name <destination>.\n",
            sftp_cmd_mv
    },
    {
        "open", true, "connect to a host",
            " [<user>@]<hostname> [<port>]\n"
            "  Establishes an SFTP connection to a given host. Only usable\n"
            "  when you are not already connected to a server.\n",
            sftp_cmd_open
    },
    {
        "put", true, "upload a file from your local machine to the server",
            " [ -r ] [ -- ] <filename> [ <remote-filename> ]\n"
            "  Uploads a file to the server and stores it there under\n"
            "  the same name, or under a different one if you supply the\n"
            "  argument <remote-filename>.\n"
            "  If -r specified, recursively store a directory.\n",
            sftp_cmd_put
    },
    {
        "pwd", true, "print your remote working directory",
            "\n"
            "  Print the current remote working directory for your SFTP session.\n",
            sftp_cmd_pwd
    },
    {
        "quit", true, "bye", NULL,
            sftp_cmd_quit
    },
    {
        "reget", true, "continue downloading files",
            " [ -r ] [ -- ] <filename> [ <local-filename> ]\n"
            "  Works exactly like the \"get\" command, but the local file\n"
            "  must already exist. The download will begin at the end of the\n"
            "  file. This is for resuming a download that was interrupted.\n"
            "  If -r specified, resume interrupted \"get -r\".\n",
            sftp_cmd_reget
    },
    {
        "ren", true, "mv", NULL,
            sftp_cmd_mv
    },
    {
        "rename", false, "mv", NULL,
            sftp_cmd_mv
    },
    {
        "reput", true, "continue uploading files",
            " [ -r ] [ -- ] <filename> [ <remote-filename> ]\n"
            "  Works exactly like the \"put\" command, but the remote file\n"
            "  must already exist. The upload will begin at the end of the\n"
            "  file. This is for resuming an upload that was interrupted.\n"
            "  If -r specified, resume interrupted \"put -r\".\n",
            sftp_cmd_reput
    },
    {
        "rm", true, "del", NULL,
            sftp_cmd_rm
    },
    {
        "rmdir", true, "remove directories on the remote server",
            " <directory-name> [ <directory-name>... ]\n"
            "  Removes the directory with the given name on the server.\n"
            "  The directory will not be removed unless it is empty.\n"
            "  Wildcards may be used to specify multiple directories.\n",
            sftp_cmd_rmdir
    }
};

const struct sftp_cmd_lookup *lookup_command(const char *name)
{
    int i, j, k, cmp;

    i = -1;
    j = lenof(sftp_lookup);
    while (j - i > 1) {
        k = (j + i) / 2;
        cmp = strcmp(name, sftp_lookup[k].name);
        if (cmp < 0)
            j = k;
        else if (cmp > 0)
            i = k;
        else {
            return &sftp_lookup[k];
        }
    }
    return NULL;
}

static int sftp_cmd_help(struct sftp_command *cmd)
{
    int i;
    if (cmd->nwords == 1) {
        /*
         * Give short help on each command.
         */
        int maxlen;
        maxlen = 0;
        for (i = 0; i < lenof(sftp_lookup); i++) {
            int len;
            if (!sftp_lookup[i].listed)
                continue;
            len = (int) strlen(sftp_lookup[i].name); // TG
            if (maxlen < len)
                maxlen = len;
        }
        for (i = 0; i < lenof(sftp_lookup); i++) {
            const struct sftp_cmd_lookup *lookup;
            if (!sftp_lookup[i].listed)
                continue;
            lookup = &sftp_lookup[i];
            printf("%-*s", maxlen+2, lookup->name);
            if (lookup->longhelp == NULL)
                lookup = lookup_command(lookup->shorthelp);
            printf("%s\n", lookup->shorthelp);
        }
    } else {
        /*
         * Give long help on specific commands.
         */
        for (i = 1; i < cmd->nwords; i++) {
            const struct sftp_cmd_lookup *lookup;
            lookup = lookup_command(cmd->words[i]);
            if (!lookup) {
                printf("help: %s: command not found\n", cmd->words[i]);
            } else {
                printf("%s", lookup->name);
                if (lookup->longhelp == NULL)
                    lookup = lookup_command(lookup->shorthelp);
                printf("%s", lookup->longhelp);
            }
        }
    }
    return 1;
}

/* ----------------------------------------------------------------------
 * Command line reading and parsing.
 */
#ifdef TGDLL
struct sftp_command *sftp_getcmd(FILE *fp, int mode, int modeflags, char *tgline) // TG 2019
#else
struct sftp_command *sftp_getcmd(FILE *fp, int mode, int modeflags)
#endif
{
    char *line;
    struct sftp_command *cmd;
    char *p, *q, *r;
    bool quoting;

    cmd = snew(struct sftp_command);
    cmd->words = NULL;
    cmd->nwords = 0;
    cmd->wordssize = 0;

    line = NULL;

#ifdef TGDLL
	if (tgline!=NULL) // TG 2019
	   line=tgline;
	else
#endif
    if (fp) {
        if (modeflags & 1)
            printf("psftp> ");
        line = fgetline(fp);
    } else {
        line = ssh_sftp_get_cmdline("psftp> ", !backend);
    }

    if (!line || !*line) {
        cmd->obey = sftp_cmd_quit;
        if ((mode == 0) || (modeflags & 1))
            printf("quit\n");
        sfree(line);
        return cmd;                    /* eof */
    }

    line[strcspn(line, "\r\n")] = '\0';

    if (modeflags & 1) {
        printf("%s\n", line);
    }

    p = line;
    while (*p && (*p == ' ' || *p == '\t'))
        p++;

    if (*p == '!') {
        /*
         * Special case: the ! command. This is always parsed as
         * exactly two words: one containing the !, and the second
         * containing everything else on the line.
         */
        cmd->nwords = 2;
        sgrowarrayn(cmd->words, cmd->wordssize, cmd->nwords, 0);
        cmd->words[0] = dupstr("!");
        cmd->words[1] = dupstr(p+1);
    } else if (*p == '#') {
        /*
         * Special case: comment. Entire line is ignored.
         */
        cmd->nwords = cmd->wordssize = 0;
    } else {

        /*
         * Parse the command line into words. The syntax is:
         *  - double quotes are removed, but cause spaces within to be
         *    treated as non-separating.
         *  - a double-doublequote pair is a literal double quote, inside
         *    _or_ outside quotes. Like this:
         *
         *      firstword "second word" "this has ""quotes"" in" and""this""
         *
         * becomes
         *
         *      >firstword<
         *      >second word<
         *      >this has "quotes" in<
         *      >and"this"<
         */
        while (1) {
            /* skip whitespace */
            while (*p && (*p == ' ' || *p == '\t'))
                p++;
            /* terminate loop */
            if (!*p)
                break;
            /* mark start of word */
            q = r = p;                 /* q sits at start, r writes word */
            quoting = false;
            while (*p) {
                if (!quoting && (*p == ' ' || *p == '\t'))
                    break;                     /* reached end of word */
                else if (*p == '"' && p[1] == '"')
                    p += 2, *r++ = '"';    /* a literal quote */
                else if (*p == '"')
                    p++, quoting = !quoting;
                else
                    *r++ = *p++;
            }
            if (*p)
                p++;                   /* skip over the whitespace */
            *r = '\0';
            sgrowarray(cmd->words, cmd->wordssize, cmd->nwords);
            cmd->words[cmd->nwords++] = dupstr(q);
        }
    }

    sfree(line);

    /*
     * Now parse the first word and assign a function.
     */

    if (cmd->nwords == 0)
        cmd->obey = sftp_cmd_null;
    else {
        const struct sftp_cmd_lookup *lookup;
        lookup = lookup_command(cmd->words[0]);
        if (!lookup)
            cmd->obey = sftp_cmd_unknown;
        else
            cmd->obey = lookup->obey;
    }

    return cmd;
}

#ifndef TGDLL
static void sftp_cmd_free(struct sftp_command *cmd)
{
    if (cmd->words) {
        for (size_t i = 0; i < cmd->nwords; i++)
            sfree(cmd->words[i]);
        sfree(cmd->words);
    }
    sfree(cmd);
}
#endif

static int do_sftp_init(void)
{
    struct sftp_packet *pktin;
    struct sftp_request *req;

    /*
     * Do protocol initialisation.
     */
    if (!fxp_init()) {
        fprintf(stderr,
                "Fatal: unable to initialise SFTP: %s\n", fxp_error());
        return 1;                      /* failure */
    }

    /*
     * Find out where our home directory is.
     */
    req = fxp_realpath_send(".");
    pktin = sftp_wait_for_reply(req);
    homedir = fxp_realpath_recv(pktin, req);

    if (!homedir) {
        fprintf(stderr,
                "Warning: failed to resolve home directory: %s\n",
                fxp_error());
        homedir = dupstr(".");
    } else {
        with_stripctrl(san, homedir)
            printf("Remote working directory is %s\n", san);
    }
    pwd = dupstr(homedir);
    return 0;
}

static void do_sftp_cleanup(void)
{
    if (backend) // TG
    {
        if (!sent_eof && backend_connected(backend)) // TG
        {
           char ch;
           backend_special(backend, SS_EOF, 0);
           sent_eof = true;
           sftp_recvdata(&ch, 1);
        } // TG
        backend_free(backend);
        sftp_cleanup_request();
        backend = NULL;
    }
    if (pwd) {
        sfree(pwd);
        pwd = NULL;
    }
    if (homedir) {
        sfree(homedir);
        homedir = NULL;
    }
}

void free_sftp_command(struct sftp_command **acmd) // TG
{
  struct sftp_command *cmd = (*acmd);

  if (cmd->words)
  {
     int i;
     for (i = 0; i < cmd->nwords; i++)
        sfree(cmd->words[i]);
     sfree(cmd->words);
  }
  sfree(cmd);

  *acmd = NULL;
}

int do_sftp(int mode, int modeflags, char *batchfile)
{
    FILE *fp;
    int ret;

    /*
     * Batch mode?
     */
    if (mode == 0) {

        /* ------------------------------------------------------------------
         * Now we're ready to do Real Stuff.
         */
        while (1) {
            struct sftp_command *cmd;
            cmd = sftp_getcmd(NULL, 0, 0, NULL); // TG 2019
            if (!cmd)
                break;
            ret = cmd->obey(cmd);
            free_sftp_command(&cmd); // TG
            if (ret < 0)
                break;
        }
    } else {
        fp = fopen(batchfile, "r");
        if (!fp) {
            printf("Fatal: unable to open %s\n", batchfile);
            return 1;
        }
        ret = 0;
        while (1) {
            struct sftp_command *cmd;
            cmd = sftp_getcmd(fp, mode, modeflags, NULL); // TG 2019
            if (!cmd)
                break;
            ret = cmd->obey(cmd);
            free_sftp_command(&cmd); // TG 2019
            if (ret < 0)
                break;
            if (ret == 0) {
                if (!(modeflags & 2))
                    break;
            }
        }
        fclose(fp);
        /*
         * In batch mode, and if exit on command failure is enabled,
         * any command failure causes the whole of PSFTP to fail.
         */
        if (ret == 0 && !(modeflags & 2)) return 2;
    }
    return 0;
}

/* ----------------------------------------------------------------------
 * Dirty bits: integration with PuTTY.
 */

// static bool verbose = false; // TG

void ldisc_echoedit_update(Ldisc *ldisc) { }

/*
 * Receive a block of data from the SSH link. Block until all data
 * is available.
 *
 * To do this, we repeatedly call the SSH protocol module, with our
 * own psftp_output() function to catch the data that comes back. We
 * do this until we have enough data.
 */
#ifdef TGDLL
#define received_data (curlibctx->received_data)
#else
static bufchain received_data;
#endif

// TG 2019: cannot put this in the libctx because freeing would
// cause crashes due to the totally C hacking implementation of sinks
// but making it threadsafe should be good enough

static TGDLLCODE(THREADVAR) BinarySink *stderr_bs;
TGDLLCODE(static THREADVAR bool thread_vars_initialized;)

TGDLLCODE(static void init_thread_vars();)

static size_t psftp_output(
    Seat *seat, bool is_stderr, const void *data, size_t len)
{
    /*
     * stderr data is just spouted to local stderr (optionally via a
     * sanitiser) and otherwise ignored.
     */
    if (is_stderr)
    {
       if (!stderr_bs || !thread_vars_initialized) // TG
          init_thread_vars(); // TG
       put_data(stderr_bs, data, len);
       return 0;
    }

    bufchain_add(&received_data, data, len);
    return 0;
}

static bool psftp_eof(Seat *seat)
{
    /*
     * We expect to be the party deciding when to close the
     * connection, so if we see EOF before we sent it ourselves, we
     * should panic.
     */
    if (!sent_eof) {
        seat_connection_fatal(
            psftp_seat, "Received unexpected end-of-file from SFTP server");
    }
    return false;
}

bool sftp_recvdata(char *buf, size_t len)
{
    // printf("sftp_recvdata len=%zd\n",len);
	uint64_t starttick=TGGetTickCount64(); // TG
    if (curlibctx->timeoutticks<1000) // TG
       curlibctx->timeoutticks=60000; // TG
    while (len > 0)
    {
        assert(backend!=NULL); // TG
        // printf("sftp_recvdata wanting %zd\n",len);
        while (bufchain_size(&received_data) == 0)
        {
            // printf("sftp_recvdata no data received, still wanting %zd\n",len);
            assert(backend!=NULL); // TG
            if (backend_exitcode(backend) >= 0 ||
                ssh_sftp_loop_iteration() < 0)
                return false;          /* doom */

            if (curlibctx->aborted) // TG
            {
                fprintf(stderr, "sftp_recvdata: aborted by program\n");
                return false;
            }

            // recalculate on every pass because
            // curlibctx->timeoutticks may be changed ad hoc by host program
            uint64_t maxtick = starttick + (curlibctx->timeoutticks / 1000 * TICKSPERSEC); // TG
            if (TGGetTickCount64()>maxtick) // TG
            {
                int elapsedseconds = (int) ((TGGetTickCount64() - starttick) / TICKSPERSEC);
                fprintf(stderr, "sftp_recvdata: timeout, no data received for %d seconds\n",elapsedseconds);
                return false;
            }
        }

        size_t got = bufchain_fetch_consume_up_to(&received_data, buf, len);
        buf += got;
        len -= got;

        if (got>0) // TG: got some Bytes - start a new 60 seconds timeout period
           starttick=TGGetTickCount64();
    }

    return true;
}
bool sftp_senddata(const char *buf, size_t len)
{
   if (backend) // TG fix AV on disconnection
   {
      backend_send(backend, buf, len);
      return true;
   }
   else
   {
      printf("not connected error in sftp_senddata\n"); // TG
      return false;
   }
}
size_t sftp_sendbuffer(void)
{
   if (backend) // TG fix AV on disconnection
      return backend_sendbuffer(backend);
   else
   {
      printf("not connected error in sftp_sendbuffer\n"); // TG
      return 0;
   }
}

/*
 *  Short description of parameters.
 */
#ifdef WITHCMDLINEXXXX
static void usage(void)
{
    printf("PuTTY Secure File Transfer (SFTP) client\n");
    printf("%s\n", ver);
    printf("Usage: psftp [options] [user@]host\n");
    printf("Options:\n");
    printf("  -V        print version information and exit\n");
    printf("  -pgpfp    print PGP key fingerprints and exit\n");
    printf("  -b file   use specified batchfile\n");
    printf("  -bc       output batchfile commands\n");
    printf("  -be       don't stop batchfile processing if errors\n");
    printf("  -v        show verbose messages\n");
    printf("  -load sessname  Load settings from saved session\n");
    printf("  -l user   connect with specified username\n");
    printf("  -P port   connect to specified port\n");
    printf("  -pw passw login with specified password\n");
    printf("  -1 -2     force use of particular SSH protocol version\n");
    printf("  -ssh -ssh-connection\n");
    printf("            force use of particular SSH protocol variant\n");
    printf("  -4 -6     force use of IPv4 or IPv6\n");
    printf("  -C        enable compression\n");
    printf("  -i key    private key file for user authentication\n");
    printf("  -noagent  disable use of Pageant\n");
    printf("  -agent    enable use of Pageant\n");
    printf("  -no-trivial-auth\n");
    printf("            disconnect if SSH authentication succeeds trivially\n");
    printf("  -hostkey keyid\n");
    printf("            manually specify a host key (may be repeated)\n");
    printf("  -batch    disable all interactive prompts\n");
    printf("  -no-sanitise-stderr  don't strip control chars from"
           " standard error\n");
    printf("  -proxycmd command\n");
    printf("            use 'command' as local proxy\n");
    printf("  -sshlog file\n");
    printf("  -sshrawlog file\n");
    printf("            log protocol details to a file\n");
    printf("  -logoverwrite\n");
    printf("  -logappend\n");
    printf("            control what happens when a log file already exists\n");
    cleanup_exit(1,true); // TG
}

static void version(void)
{
  char *buildinfo_text = buildinfo("\n");
  printf("psftp: %s\n%s\n", ver, buildinfo_text);
  sfree(buildinfo_text);
  exit(0);
}
#endif
/*
 * Connect to a host.
 */
static int psftp_connect(char *userhost, char *user, int portnumber)
{
	CP("psftp_c1");
	printf("psftp_connect connecting with %s, port %d, as user %s.\n",userhost,portnumber,user); // TG

	char *host=NULL, *realhost=NULL; // TG 2021: fix crash when attempting to free invalid realhost
	const char *err=NULL; // TG

    /* Separate host and username */
    host = userhost;
    host = strrchr(host, '@');
    if (host == NULL) {
        host = userhost;
    } else {
        *host++ = '\0';
        if (user) {
            printf("psftp: multiple usernames specified; using \"%s\"\n",
                   user);
        } else
            user = userhost;
    }

	CP("psftp_c2");
    /*
     * If we haven't loaded session details already (e.g., from -load),
     * try looking for a session called "host".
     */
    if (!cmdline_loaded_session()) {
		CP("psftp_c3");
        /* Try to load settings for `host' into a temporary config */
        Conf *conf2 = conf_new();
        conf_set_str(conf2, CONF_host, "");
        do_defaults(host, conf2);
        if (conf_get_str(conf2, CONF_host)[0] != '\0') {
            /* Settings present and include hostname */
            /* Re-load data into the real config. */
            do_defaults(host, conf);
        } else {
            /* Session doesn't exist or mention a hostname. */
            /* Use `host' as a bare hostname. */
            conf_set_str(conf, CONF_host, host);
        }
        conf_free(conf2);
    } else {
		CP("psftp_c4");
        /* Patch in hostname `host' to session details. */
        conf_set_str(conf, CONF_host, host);
    }

    /*
     * Force protocol to SSH if the user has somehow contrived to
     * select one we don't support (e.g. by loading an inappropriate
     * saved session). In that situation we assume the port number is
     * useless too.)
     */
	CP("psftp_c5");
    if (!backend_vt_from_proto(conf_get_int(conf, CONF_protocol))) {
		CP("psftp_c6");
        conf_set_int(conf, CONF_protocol, PROT_SSH);
        conf_set_int(conf, CONF_port, 22);
    }

    /*
     * If saved session / Default Settings says SSH-1 (`1 only' or `1'),
     * then change it to SSH-2, on the grounds that that's more likely to
     * work for SFTP. (Can be overridden with `-1' option.)
     * But if it says `2 only' or `2', respect which.
     */
	CP("psftp_c7");
    if ((conf_get_int(conf, CONF_sshprot) & ~1) != 2)   /* is it 2 or 3? */
        conf_set_int(conf, CONF_sshprot, 2);

    /*
     * Enact command-line overrides.
     */
	CP("psftp_c8");
    cmdline_run_saved(conf);

    /*
     * Muck about with the hostname in various ways.
     */
    {
		CP("psftp_c9");
        char *hostbuf = dupstr(conf_get_str(conf, CONF_host));
        char *host = hostbuf;
        char *p, *q;

        /*
         * Trim leading whitespace.
         */
        host += strspn(host, " \t");

        /*
         * See if host is of the form user@host, and separate out
         * the username if so.
         */
        if (host[0] != '\0') {
            char *atsign = strrchr(host, '@');
            if (atsign) {
                *atsign = '\0';
                conf_set_str(conf, CONF_username, host);
                host = atsign + 1;
            }
        }

        /*
         * Remove any remaining whitespace.
         */
        p = hostbuf;
        q = host;
        while (*q) {
            if (*q != ' ' && *q != '\t')
                *p++ = *q;
            q++;
        }
        *p = '\0';

        conf_set_str(conf, CONF_host, hostbuf);
        sfree(hostbuf);
    }

    /* Set username */
	CP("psftp_c10");
    if (user != NULL && user[0] != '\0') {
        conf_set_str(conf, CONF_username, user);
    }

    if (portnumber)
        conf_set_int(conf, CONF_port, portnumber);

    /*
     * Disable scary things which shouldn't be enabled for simple
     * things like SCP and SFTP: agent forwarding, port forwarding,
     * X forwarding.
     */
	CP("psftp_c11");
	NORMALCODE(conf_set_bool(conf, CONF_x11_forward, false);) // TG
    conf_set_bool(conf, CONF_agentfwd, false);
    conf_set_bool(conf, CONF_ssh_simple, true);
    {
        char *key;
        while ((key = conf_get_str_nthstrkey(conf, CONF_portfwd, 0)) != NULL)
            conf_del_str_str(conf, CONF_portfwd, key);
    }

    /* Set up subsystem name. */
	CP("psftp_c12");
    conf_set_str(conf, CONF_remote_cmd, "sftp");
    conf_set_bool(conf, CONF_ssh_subsys, true);
    conf_set_bool(conf, CONF_nopty, true);

    /*
     * Set up fallback option, for SSH-1 servers or servers with the
     * sftp subsystem not enabled but the server binary installed
     * in the usual place. We only support fallback on Unix
     * systems, and we use a kludgy piece of shellery which should
     * try to find sftp-server in various places (the obvious
     * systemwide spots /usr/lib and /usr/local/lib, and then the
     * user's PATH) and finally give up.
     *
     *   test -x /usr/lib/sftp-server && exec /usr/lib/sftp-server
     *   test -x /usr/local/lib/sftp-server && exec /usr/local/lib/sftp-server
     *   exec sftp-server
     *
     * the idea being that this will attempt to use either of the
     * obvious pathnames and then give up, and when it does give up
     * it will print the preferred pathname in the error messages.
     */
    conf_set_str(conf, CONF_remote_cmd2,
                 "test -x /usr/lib/sftp-server &&"
                 " exec /usr/lib/sftp-server\n"
                 "test -x /usr/local/lib/sftp-server &&"
                 " exec /usr/local/lib/sftp-server\n"
                 "exec sftp-server");
    conf_set_bool(conf, CONF_ssh_subsys2, false);

	CP("psftp_c13");
	if (psftp_logctx==NULL) // TG - might connect again after disconnecting
	{
    psftp_logctx = log_init(console_cli_logpolicy, conf);
#ifdef DEBUG_MALLOC
	   printf("Created new logctx.\n"); // TG
#endif
	}
#ifdef DEBUG_MALLOC
	else
	   printf("Reusing logctx.\n"); // TG
#endif

	CP("psftp_c14");
    platform_psftp_pre_conn_setup(console_cli_logpolicy);

	CP("psftp_c15");
    err = backend_init(backend_vt_from_proto(
                           conf_get_int(conf, CONF_protocol)),
                       psftp_seat, &backend, psftp_logctx, conf,
                       conf_get_str(conf, CONF_host),
                       conf_get_int(conf, CONF_port),
                       &realhost, 0,
                       conf_get_bool(conf, CONF_tcp_keepalives));
    if (err != NULL) {
		CP("psftp_c16");
        fprintf(stderr, "ssh_init: %s\n", err);
		if (realhost != NULL) // TG
		   sfree(realhost); // TG
        return 1;
    }

	CP("psftp_c17");
	uint64_t starttick=TGGetTickCount64(); // TG
	if (curlibctx->connectiontimeoutticks<1000) // TG
	   curlibctx->connectiontimeoutticks=60000; // TG

	CP("psftp_c18");
	while (!backend_sendok(backend))
	{
		CP("psftp_c20");
		if (curlibctx->aborted) // TG
		{
			CP("psftp_c21");
			fprintf(stderr, "ssh_init: aborted by program\n"); // TG
			if (realhost != NULL)
			   sfree(realhost);
			CP("psftp_c22");
			return 1;
		}
		// recalculate on every pass because
		// curlibctx->connectiontimeoutticks may be changed ad hoc by host program
		CP("psftp_c23");
		uint64_t maxtick = starttick + (curlibctx->connectiontimeoutticks / 1000 * TICKSPERSEC); // TG
		if (TGGetTickCount64()>maxtick)
		{
			CP("psftp_c24");
			int elapsedseconds = (int) ((TGGetTickCount64() - starttick) / TICKSPERSEC); // TG
			fprintf(stderr, "ssh_init: timeout, no connection after %d seconds\n",elapsedseconds);
			if (realhost != NULL)
			   sfree(realhost);
			return 1;
		}
		CP("psftp_c25");
		if (backend_exitcode(backend) >= 0)
		{
			CP("psftp_c26");
			if (realhost != NULL) // TG
			   sfree(realhost);
			CP("psftp_c27");
			return 1;
		}
		CP("psftp_c28");
		if (ssh_sftp_loop_iteration() < 0)
		{
			CP("psftp_c29");
			fprintf(stderr, "ssh_init: error during SSH connection setup\n");
			if (realhost != NULL) // TG
			   sfree(realhost);
			CP("psftp_c30");
			return 1;
		}
	}
	CP("psftp_c31");
	if (verbose && realhost != NULL)
		printf("Connected to %s\n", realhost);
	if (realhost != NULL)
	{
	    CP("psftp_c35");
		sfree(realhost);
	}
	CP("psftp_c40");
	return 0;
}

void cmdline_error(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "psftp: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fprintf(stderr, "\n       try typing \"psftp -h\" for help\n");
    exit(1);
}

const bool share_can_be_downstream = true;
const bool share_can_be_upstream = false;

// TG 2019: cannot put these in the libctx because freeing would
// cause crashes due to the totally C hacking implementations of sinks and StripCtrlChars
// but making them threadsafe should be good enough
static TGDLLCODE(THREADVAR) StripCtrlChars *stderr_scc;
static TGDLLCODE(THREADVAR) stdio_sink stderr_ss;

static void init_thread_vars() // TG
{
   if (!thread_vars_initialized)
   {
      //printf("init_thread_vars(), Thread ID: %ud\n",GetCurrentThreadId());
      stdio_sink_init(&stderr_ss, stderr);
      stderr_bs = BinarySink_UPCAST(&stderr_ss);

      #ifndef _WINDOWS
      uxsel_init();
      #endif

      thread_vars_initialized=true;
   }
   //else
   //   printf("init_thread_vars() redundant call, Thread ID: %ud\n",GetCurrentThreadId());

   #ifndef _WINDOWS
   if (!curlibctx->fds) // just to double-check
      uxsel_init();
   #endif
}

static void free_thread_vars() // TG
{
   if (thread_vars_initialized)
   {
	  //printf("calling stripctrl_free(stderr_scc)\n");
	  stripctrl_free(stderr_scc);
	  thread_vars_initialized=false;

	  #ifndef _WINDOWS
	  uxsel_free();
	  #endif
   }
}

#if defined(_MSC_VER)
	//  Microsoft
	#define EXPORT __declspec(dllexport)
	#define IMPORT __declspec(dllimport)
#elif defined(__GNUC__)
	//  GCC
	#define EXPORT __attribute__((visibility("default")))
	#define IMPORT
#else
	//  do nothing and hope for the best?
	#define EXPORT
	#define IMPORT
	#pragma warning Unknown dynamic link import/export semantics.
#endif

const unsigned cmdline_tooltype = TOOLTYPE_FILETRANSFER;

/*
 * Main program. Parse arguments etc.
 */
#ifdef WITHCMDLINEXXXX
TGDLLCODE(EXPORT) int psftp_main(int argc, char *argv[]) // TG 2019
{
    int i, ret;
    int portnumber = 0;
    char *userhost, *user;
    int mode = 0;
    int modeflags = 0;
    bool sanitise_stderr = true;
    char *batchfile = NULL;

    sk_init();

    userhost = user = NULL;

    /* Load Default Settings before doing anything else. */
    conf = conf_new();
    do_defaults(NULL, conf);

    for (i = 1; i < argc; i++) {
        int ret;
        if (argv[i][0] != '-') {
            if (userhost)
                usage();
            else
                userhost = dupstr(argv[i]);
            continue;
        }
        ret = cmdline_process_param(argv[i], i+1<argc?argv[i+1]:NULL, 1, conf);
        if (ret == -2) {
            cmdline_error("option \"%s\" requires an argument", argv[i]);
        } else if (ret == 2) {
            i++;               /* skip next argument */
        } else if (ret == 1) {
            /* We have our own verbosity in addition to `flags'. */
            if (cmdline_verbose())
                verbose = true;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "-?") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage();
        } else if (strcmp(argv[i], "-pgpfp") == 0) {
            pgp_fingerprints();
            return 1;
        } else if (strcmp(argv[i], "-V") == 0 ||
                   strcmp(argv[i], "--version") == 0) {
            version();
        } else if (strcmp(argv[i], "-batch") == 0) {
            console_batch_mode = true;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            mode = 1;
            batchfile = argv[++i];
        } else if (strcmp(argv[i], "-bc") == 0) {
            modeflags = modeflags | 1;
        } else if (strcmp(argv[i], "-be") == 0) {
            modeflags = modeflags | 2;
        } else if (strcmp(argv[i], "-sanitise-stderr") == 0) {
            sanitise_stderr = true;
        } else if (strcmp(argv[i], "-no-sanitise-stderr") == 0) {
            sanitise_stderr = false;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            cmdline_error("unknown option \"%s\"", argv[i]);
        }
    }
    argc -= i;
    argv += i;
    backend = NULL;

    stdio_sink_init(&stderr_ss, stderr);
    stderr_bs = BinarySink_UPCAST(&stderr_ss);
    if (sanitise_stderr) {
        stderr_scc = stripctrl_new(stderr_bs, false, L'\0');
        stderr_bs = BinarySink_UPCAST(stderr_scc);
    }

 // TG
 // TG
    /*
     * If the loaded session provides a hostname, and a hostname has not
     * otherwise been specified, pop it in `userhost' so that
     * `psftp -load sessname' is sufficient to start a session.
     */
    if (!userhost && conf_get_str(conf, CONF_host)[0] != '\0') {
        userhost = dupstr(conf_get_str(conf, CONF_host));
    }

    /*
     * If a user@host string has already been provided, connect to
     * it now.
     */
    if (userhost) {
        int ret;
        ret = psftp_connect(userhost, user, portnumber);
        sfree(userhost);
        if (ret)
            return 1;
        if (do_sftp_init())
            return 1;
    } else {
		printf("psftp: no hostname specified\n"); // TG
    }

    ret = do_sftp(mode, modeflags, batchfile);

    if (backend && backend_connected(backend)) {
        char ch;
        backend_special(backend, SS_EOF, 0);
        sent_eof = true;
        sftp_recvdata(&ch, 1);
    }
    do_sftp_cleanup();
    random_save_seed();
    cmdline_cleanup();
    sk_cleanup(true); // TG

 // TG
    stripctrl_free(stderr_scc);

    if (psftp_logctx)
    {
        log_free(psftp_logctx);
        psftp_logctx = NULL; // TG
    }

    return ret;
}
#endif

// TG: functions for external programs
EXPORT int tggetlibrarycontextsize() // TG 2019
{
  TTGLibraryContext x;
  return sizeof x;
}

EXPORT void tggetstructsizes(int *Pulongsize,int *Pnamesize,int *Pattrsize,int *Pnamessize) // TG 2019
{
    struct fxp_attrs attrs;
    struct fxp_names names;
    struct fxp_name name;
    unsigned long usl=0;

    *Pulongsize = sizeof usl;
    *Pnamesize =  sizeof name;
    *Pattrsize =  sizeof attrs;
    *Pnamessize = sizeof names;
}

bool cmdline_seat_verbose(Seat *seat) // TG
{
  return verbose;
}

bool cmdline_lp_verbose(LogPolicy *lp) // TG
{
  return verbose;
}

bool cmdline_loaded_session(void) // TG
{
  return false; // we don't preload sessions
}


EXPORT int tgputty_initcontext(const char averbose,TTGLibraryContext *libctx)
{
    curlibctx=libctx;
    ContextCounter++;
    ThreadContextCounter++;
	verbose=(averbose & 1) == 1;
	checkpoints=(averbose & 2) == 2;

	if (ThreadContextCounter==1)
       init_thread_vars();

    libctx->bufchainlength = sizeof received_data;

    if (libctx->structsize<tggetlibrarycontextsize())
    {
       printf("Incorrect TGLibraryContext struct size");
       if (curlibctx->raise_exception_callback)
          curlibctx->raise_exception_callback("Incorrect TGLibraryContext struct size",__FILE__,__LINE__,curlibctx);
       return -101;
    }

	libctx->mode = 0;
	libctx->modeflags = 0;
	libctx->batchfile = NULL;
#ifdef _WINDOWS
	libctx->winselcli_event = INVALID_HANDLE_VALUE;
#endif

	// flags = (verbose ? FLAG_VERBOSE : 0)
#ifdef FLAG_SYNCAGENT
		| FLAG_SYNCAGENT
#endif
		;
	// cmdline_tooltype = TOOLTYPE_FILETRANSFER;
	sk_init();

	/* Load Default Settings before doing anything else. */
	conf = conf_new();
	do_defaults(NULL, conf);
	// loaded_session = false;

    // initial values taken from sshcommon.c
    libctx->pktin_freeq_head.next = &libctx->pktin_freeq_head;
    libctx->pktin_freeq_head.prev = &libctx->pktin_freeq_head;
    libctx->pktin_freeq_head.on_free_queue = true;

    libctx->ic_pktin_free.fn = pktin_free_queue_callback;

    backend = NULL;

    return 0;
}


#ifdef WITHCMDLINEXXXX
EXPORT int tgputty_initwithcmdline(int argc, char *argv[], TTGLibraryContext *libctx) // TG 2019
{
    int res=tgputty_initcontext(0,libctx);
    if (res!=0)
       return res;

	int i;
	int portnumber = 0;
	char *userhost, *user;

	userhost = user = NULL;
	bool sanitise_stderr = true;

	for (i = 1; i < argc; i++) {
		int ret;
		if (argv[i][0] != '-') {
			if (userhost)
				usage();
			else
				userhost = dupstr(argv[i]);
			continue;
		}
		ret = cmdline_process_param(argv[i], i+1<argc?argv[i+1]:NULL, 1, conf);
		if (ret == -2) {
			cmdline_error("option \"%s\" requires an argument", argv[i]);
		} else if (ret == 2) {
			i++;               /* skip next argument */
		} else if (ret == 1) {
			/* We have our own verbosity in addition to `flags'. */
			//if (flags & FLAG_VERBOSE)
			//	verbose = true;
		} else if (strcmp(argv[i], "-h") == 0 ||
				   strcmp(argv[i], "-?") == 0 ||
				   strcmp(argv[i], "--help") == 0) {
			usage();
		} else if (strcmp(argv[i], "-pgpfp") == 0) {
			pgp_fingerprints();
			return 1;
		} else if (strcmp(argv[i], "-V") == 0 ||
				   strcmp(argv[i], "--version") == 0) {
			version();
		} else if (strcmp(argv[i], "-batch") == 0) {
			console_batch_mode = true;
		} else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
			libctx->mode = 1;
			libctx->batchfile = argv[++i];
		} else if (strcmp(argv[i], "-bc") == 0) {
			libctx->modeflags = libctx->modeflags | 1;
		} else if (strcmp(argv[i], "-be") == 0) {
			libctx->modeflags = libctx->modeflags | 2;
		} else if (strcmp(argv[i], "-sanitise-stderr") == 0) {
			sanitise_stderr = true;
		} else if (strcmp(argv[i], "-no-sanitise-stderr") == 0) {
			sanitise_stderr = false;
		} else if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		} else {
			cmdline_error("unknown option \"%s\"", argv[i]);
		}
	}
	argc -= i;
	argv += i;

    // TO DO: not thread safe, but this function tgputty_initwithcmdline is
    // not supposed to be called by multithreaded apps anyway
	if (sanitise_stderr)
    {
		stderr_scc = stripctrl_new(stderr_bs, false, L'\0');
		stderr_bs = BinarySink_UPCAST(stderr_scc);
	}

	/*
	 * If the loaded session provides a hostname, and a hostname has not
	 * otherwise been specified, pop it in `userhost' so that
	 * `psftp -load sessname' is sufficient to start a session.
	 */
	if (!userhost && conf_get_str(conf, CONF_host)[0] != '\0') {
		userhost = dupstr(conf_get_str(conf, CONF_host));
	}

	/*
	 * If a user@host string has already been provided, connect to
	 * it now.
	 */
	if (userhost) {
		int ret;
		ret = psftp_connect(userhost, user, portnumber);
		sfree(userhost);
		if (ret)
			return 1;
		if (do_sftp_init())
			return 1;
	} else {
		printf("psftp: no hostname specified\n");
	}
    return 0;
}
#endif

EXPORT int tgputtyrunpsftp(TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  return do_sftp(libctx->mode, libctx->modeflags, libctx->batchfile);
}

EXPORT void tgputtysetappname(const char *newappname,const char *appversion) // TG 2019
{
  appname = dupstr(newappname);
  ver = dupstr(appversion);
  sshver = malloc(strlen(ver)+2);
  sshver[0] = '-';
  strcpy(sshver+1,ver);

  for (int i=0;i<strlen(sshver);i++)
	if (sshver[i]==' ')
       sshver[i]='-';
}

EXPORT int tgputtysftpcommand(const char *line, TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  // make a copy of the command line string
  // because sftp_getcmd modifies it and we
  // do not want it to write to caller's memory
  // in fact it can cause an AV if the caller passes a constant
  char *linebuf = dupstr(line);

  // code from do_sftp
  struct sftp_command *cmd;
  cmd = sftp_getcmd(NULL, 0, 0, linebuf);
  // linebuf is freed by sftp_getcmd

  if (!cmd)
	 return 2;
  int ret = cmd->obey(cmd);

  free_sftp_command(&cmd);
  return ret;
}

EXPORT int tgsftp_connect(const char *ahost,const char *auser,const int aport,const char *apassword,
										 TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  CP("tgsftp_connect");

  #ifndef _WINDOWS
  if (!thread_vars_initialized || !curlibctx->fds)
  {
     CP("sftpcn10");
	 init_thread_vars(); // TG ... because Unix doesn't have a DLL_THREAD_ATTACH feature
     CP("sftpcn11");
  }
  #endif

  CP("sftpcn12");
  printf("Connecting with %s, port %d, as user %s.\n",ahost,aport,auser);

  CP("sftpcn13");
  libctx->caller_supplied_password = dupstr(apassword);

  char *ourhost=dupstr(ahost);
  char *ouruser=dupstr(auser);

  CP("sftpcn20");
  int result=psftp_connect(ourhost,ouruser,aport);
  CP("sftpcn21");
  printf("psftp_connect result is %d\n",result);

  if (ourhost)
	 sfree(ourhost);
  if (ouruser)
	 sfree(ouruser);

  CP("sftpcn22");
  if (libctx->caller_supplied_password!=NULL)
  {
	 sfree(libctx->caller_supplied_password);
	 libctx->caller_supplied_password=NULL;
  }

  if (result==0)
  {
	 CP("sftpcn30");
	 result=do_sftp_init();
	 CP("sftpcn31");
	 printf("do_sftp_init result is %d\n",result);
  }
  else
  {
	CP("sftpcn40");
#ifdef DEBUG_MALLOC
	printf("connect failed, calling do_sftp_cleanup, backend=%p, connected=%d\n",
		   backend,
		   backend ? backend_connected(backend): false);
#endif
	do_sftp_cleanup();
	CP("sftpcn45");
  }

  printf("tgsftp_connect final result is %d\n",result);
  CP("sftpcn49X");
  return result;
}


EXPORT int tgsftp_cd(const char *adir,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  struct sftp_command *cmd = snew(struct sftp_command);
  cmd->words = NULL;
  cmd->nwords = 2;
  cmd->wordssize = 0;

  sgrowarrayn(cmd->words, cmd->wordssize, cmd->nwords, 0);
  cmd->words[0] = dupstr("cd");
  cmd->words[1] = dupstr(adir);

  int result=sftp_cmd_cd(cmd);

  free_sftp_command(&cmd);

  return result;
}

EXPORT int tgsftp_rm(const char *afile,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  char *therealname = canonify(afile);
  int result=sftp_action_rm(NULL,therealname);
  free(therealname);
  return result;
}

EXPORT int tgsftp_rmdir(const char *adir,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  char *therealname = canonify(adir);
  int result=sftp_action_rmdir(NULL,therealname);
  free(therealname);
  return result;
}

EXPORT int tgsftp_ls(const char *adir,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  struct sftp_command *cmd = snew(struct sftp_command);
  cmd->words = NULL;
  cmd->wordssize = 0;
  if ((adir!=NULL) && (strlen(adir)>0))
  {
	 cmd->nwords = 2;
	 sgrowarrayn(cmd->words, cmd->wordssize, cmd->nwords, 0);
	 cmd->words[0] = dupstr("ls");
	 cmd->words[1] = dupstr(adir);
  }
  else
	 cmd->nwords = 0;

  int result=sftp_cmd_ls(cmd);
  free_sftp_command(&cmd);
  return result;
}

EXPORT int tgsftp_mkdir(const char *adir,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  struct sftp_command *cmd = snew(struct sftp_command);
  cmd->words = NULL;
  cmd->nwords = 2;
  cmd->wordssize = 0;

  sgrowarrayn(cmd->words, cmd->wordssize, cmd->nwords, 0);
  cmd->words[0] = dupstr("mkdir");
  cmd->words[1] = dupstr(adir);

  int result=sftp_cmd_mkdir(cmd);
  free_sftp_command(&cmd);
  return result;
}


EXPORT int tgsftp_mv(const char *afrom,const char *ato,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  struct sftp_command *cmd = snew(struct sftp_command);
  cmd->words = NULL;
  cmd->nwords = 3;
  cmd->wordssize = 0;

  sgrowarrayn(cmd->words, cmd->wordssize, cmd->nwords, 0);
  cmd->words[0] = dupstr("mv");
  cmd->words[1] = dupstr(afrom);
  cmd->words[2] = dupstr(ato);

  int result=sftp_cmd_mv(cmd);
  free_sftp_command(&cmd);
  return result;
}

EXPORT int tgsftp_mvex(const char *afrom,const char *ato,const int moveflags,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  struct sftp_command *cmd = snew(struct sftp_command);
  cmd->words = NULL;
  cmd->nwords = 3;
  cmd->wordssize = 0;

  sgrowarrayn(cmd->words, cmd->wordssize, cmd->nwords, 0);
  cmd->words[0] = dupstr("mv");
  cmd->words[1] = dupstr(afrom);
  cmd->words[2] = dupstr(ato);

  int result=sftp_cmd_mvex(cmd,moveflags);
  free_sftp_command(&cmd);
  return result;
}

EXPORT int tgsftp_getstat(const char *afrom,struct fxp_attrs *attrs,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  char *fname = canonify(afrom);

  int res=get_stat(fname,attrs);

  free(fname);
  return res;
}

EXPORT int tgsftp_setstat(const char *afrom,struct fxp_attrs *attrs,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  char *fname = canonify(afrom);

  int res=set_stat(fname,attrs);

  free(fname);
  return res;
}

EXPORT int tgsftp_putfile(const char *afromfile,const char *atofile,const bool anappend,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  char *fromfile=dupstr(afromfile);
  char *outfname = canonify(atofile);

  int result=sftp_put_file(fromfile, outfname, false /*recurse*/, anappend);

  sfree(outfname);
  sfree(fromfile);

  return result;
}

EXPORT int tgsftp_getfile(const char *afromfile,const char *atofile,const bool anappend,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  char *tofile=dupstr(atofile);
  char *infname = canonify(afromfile);

  int result=sftp_get_file(infname, tofile, false /*recurse*/, anappend);

  sfree(infname);
  sfree(tofile);

  return result;
}

EXPORT void tgsftp_close(TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  sftp_cmd_close(NULL);
}

EXPORT void tgputty_setverbose(const char averbose) // TG 2019
{
  verbose = (averbose & 1) == 1;
  checkpoints = (averbose & 2) == 2;
  // flags = (verbose ? FLAG_VERBOSE : 0);
}

EXPORT void tgputty_setkeyfile(const char *apathname,TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  Filename *fn = filename_from_str(apathname);
  conf_set_filename(conf, CONF_keyfile, fn);
  filename_free(fn);
}

EXPORT struct fxp_handle *tgputty_openfile(const char *apathname,
                                                          const int anopenflags,
                                                          const struct fxp_attrs *attrs,
                                                          TTGLibraryContext *libctx) // TG 2019
{
   curlibctx=libctx;
#ifdef DEBUG_UPLOAD
   printf("Opening file %s\n",apathname);
#endif
   struct sftp_request *req = fxp_open_send(apathname,anopenflags,attrs);
   struct sftp_packet *pktin = sftp_wait_for_reply(req);
   return fxp_open_recv(pktin, req);
}

EXPORT int tgputty_closefile(struct fxp_handle **fh,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   assert(fh != NULL);
   assert((*fh) != NULL);
#ifdef DEBUG_UPLOAD
   printf("Closing file.\n");
#endif
   struct sftp_request *req = fxp_close_send(*fh);
   (*fh) = NULL; // prevent AV when close is called another time by host program
   struct sftp_packet *pktin = sftp_wait_for_reply(req);
   return fxp_close_recv(pktin, req);
}

EXPORT void *tgputty_xfer_upload_init(struct fxp_handle *fh, uint64_t offset,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
#ifdef DEBUG_UPLOAD
  printf("calling xfer_upload_init with offset %" PRIu64 "\n",offset);
#endif
  return xfer_upload_init(fh,offset);
}

EXPORT void *tgputty_xfer_download_init(struct fxp_handle *fh, uint64_t offset,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
#ifdef DEBUG_DOWNLOAD
  printf("calling xfer_download_init with offset %" PRIu64 "\n",offset);
#endif
  return xfer_download_init(fh,offset);
}

EXPORT bool tgputty_xfer_download_preparequeue(struct fxp_xfer *xfer,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
  xfer_download_queue(xfer);
  struct sftp_packet *pktin = sftp_recv();

  int retd = xfer_download_gotpkt(xfer, pktin);

  if (retd <= 0)
  {
	 printf("error while reading: %s\n", fxp_error());
	 if (retd == INT_MIN)        /* pktin not even freed */
		sfree(pktin);
	 return false;
  }

  return true;
}

EXPORT bool tgputty_xfer_upload_ready(struct fxp_xfer *xfer,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
  return xfer_upload_ready(xfer);
}

EXPORT void tgputty_xfer_upload_data(struct fxp_xfer *xfer, char *buffer, int len, uint64_t anoffset,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
  //printf("calling xfer_set_offset, anoffset is %" PRIu64 "\n",anoffset);
  xfer_set_offset(xfer,anoffset);
#ifdef DEBUG_UPLOAD
  printf("calling xfer_upload_data, len is %d\n",len);
#endif
  xfer_upload_data(xfer,buffer,len);
}

EXPORT bool tgputty_xfer_download_data(struct fxp_xfer *xfer, void **buffer, int *len, TTGLibraryContext *libctx)
{
  curlibctx=libctx;
#ifdef DEBUG_DOWNLOAD
  printf("calling xfer_download_data\n");
#endif
  return xfer_download_data(xfer,buffer,len);
}

EXPORT void tgputty_xfer_set_error(struct fxp_xfer *xfer,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
  xfer_set_error(xfer);
}

EXPORT bool tgputty_xfer_ensuredone(struct fxp_xfer *xfer,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
  bool err=false;
  // code taken from sftp_put_file
#ifdef DEBUG_UPLOAD
  printf("ensure xfer_done\n");
#endif
  if (!xfer_done(xfer))
  {
     struct sftp_packet *pktin=sftp_recv();
     if (pktin)
     {
#ifdef DEBUG_UPLOAD
        printf("calling xfer_upload_gotpkt\n");
#endif
        int ret = xfer_upload_gotpkt(xfer, pktin);
        if (ret <= 0)
        {
           if (ret == INT_MIN)        /* pktin not even freed */
              sfree(pktin);
           if (!err)
           {
              printf("error while writing: %s\n", fxp_error());
              err = true;
           }
        }
     }
     else
     {
        printf("Disconnection detected (pktin==NULL)\n");
        err = true;
     }
  }
#ifdef DEBUG_UPLOAD
  else
     printf("xfer_done=true\n");
#endif
  return !err;
}

EXPORT bool tgputty_xfer_done(struct fxp_xfer *xfer,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
  return xfer_done(xfer);
}

EXPORT void tgputty_xfer_cleanup(struct fxp_xfer *xfer,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
#ifdef DEBUG_UPLOAD
  printf("calling xfer_cleanup\n");
#endif
  xfer_cleanup(xfer);
}

EXPORT void tgputty_sfree(void *p,TTGLibraryContext *libctx)
{
  curlibctx=libctx;
  sfree(p);
}


EXPORT void tgputtygetversions(double *puttyrelease,int *tgputtylibbuild) // TG 2019
{
  *puttyrelease = RELEASE;
  *tgputtylibbuild = TGDLLBUILDNUM;
}


EXPORT void tgputtyfree(TTGLibraryContext *libctx) // TG 2019
{
  curlibctx=libctx;
  if (backend && backend_connected(backend))
  {
      char ch;
      backend_special(backend, SS_EOF, 0);
      sent_eof = true;
      sftp_recvdata(&ch, 1);
  }

  printf("calling do_sftp_cleanup()\n");
  do_sftp_cleanup();

  if (ContextCounter==1)
  {
	 printf("calling random_save_seed()\n");
	 random_save_seed();
	 printf("calling cmdline_cleanup()\n");
	 cmdline_cleanup();
  }
  printf("calling sk_cleanup()\n");
  sk_cleanup(false);

  printf("almost done\n");

  if (psftp_logctx)
  {
     log_free(psftp_logctx);
     psftp_logctx = NULL;
  }

#ifdef _WINDOWS
  if ((libctx->winselcli_event!=0) && (libctx->winselcli_event!=INVALID_HANDLE_VALUE))
	 CloseHandle(libctx->winselcli_event);

  if (libctx->winselcli_sockets)
  {
	 freetree234(libctx->winselcli_sockets);
	 libctx->winselcli_sockets=NULL;
  }
#endif

  conf_free(conf);
  if (libctx->timers)
  {
     freetree234(libctx->timers);
     libctx->timers = NULL;
  }
  if (libctx->timer_contexts)
  {
     freetree234(libctx->timer_contexts);
     libctx->timer_contexts = NULL;
  }

  free_thread_vars();

  ContextCounter--;
  ThreadContextCounter--;
  curlibctx=NULL;
  return;
}

EXPORT bool tgputty_getconfigarrays(const void **types,const void **subtypes,const void **names,int *count)
{
  (*types) = valuetypes;
  (*subtypes) = subkeytypes;
  (*names) = confnames;
  (*count) = MAXCONFKEY+1;
  return true;
}

EXPORT bool tgputty_conf_get_bool(int key,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   return conf_get_bool(conf,key);
}

EXPORT int tgputty_conf_get_int(int key,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   return conf_get_int(conf,key);
}

EXPORT int tgputty_conf_get_int_int(int key, int subkey,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   return conf_get_int_int(conf,key,subkey);
}

EXPORT char *tgputty_conf_get_str(int key,TTGLibraryContext *libctx)   /* result still owned by conf */
{
   curlibctx=libctx;
   return conf_get_str(conf,key);
}

EXPORT char *tgputty_conf_get_str_str(int key, const char *subkey,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   return conf_get_str_str(conf,key,subkey);
}

EXPORT void tgputty_conf_set_bool(int key, bool value,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   conf_set_bool(conf,key,value);
}

EXPORT void tgputty_conf_set_int(int key, int value,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   conf_set_int(conf,key,value);
}

EXPORT void tgputty_conf_set_int_int(int key, int subkey, int value,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   conf_set_int_int(conf,key,subkey,value);
}

EXPORT void tgputty_conf_set_str(int key, const char *value,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   conf_set_str(conf,key,value);
}

EXPORT void tgputty_conf_set_str_str(int key,const char *subkey, const char *value,TTGLibraryContext *libctx)
{
   curlibctx=libctx;
   conf_set_str_str(conf,key,subkey,value);
}


// TG 2019, for DLL use
// similar to cmdline_get_passwd_input in cmdline.c
static int tg_get_userpass_input(Seat *seat, prompts_t *p, bufchain *input)
{
	if (!curlibctx->caller_supplied_password ||
		(strlen(curlibctx->caller_supplied_password)==0) ||
		curlibctx->tried_caller_supplied_password_once ||
		(p->n_prompts != 1) ||
		p->prompts[0]->echo)
	{
	   if (curlibctx->getpassword_callback)
	   {
		  int i;
		  for (i = 0; i < (int)p->n_prompts; i++)
			prompt_set_result(p->prompts[i], "");

		  for (int curr_prompt = 0; curr_prompt < p->n_prompts; curr_prompt++)
		  {
			 prompt_t *pr = p->prompts[curr_prompt];
			 bool cancel=false;
			 const char *thepwd = curlibctx->getpassword_callback(pr->prompt,pr->echo,&cancel,curlibctx);

			 if (cancel)
				return 0;
             prompt_set_result(pr,thepwd);
		  }
          return 1;
	   }
	   else
		  return filexfer_get_userpass_input(seat,p,input); // pass on to the original function
	}

	prompt_set_result(p->prompts[0], curlibctx->caller_supplied_password);
	smemclr(curlibctx->caller_supplied_password, strlen(curlibctx->caller_supplied_password));
	sfree(curlibctx->caller_supplied_password);
	curlibctx->caller_supplied_password = NULL;
	curlibctx->tried_caller_supplied_password_once = true;
	return 1;
}

#undef printf
#undef fprintf
#undef fflush
#undef fwrite

#define printbufsize 300
THREADVAR char printbuf[printbufsize];
THREADVAR size_t printbufpos;

char *printnow(const char *msg,bool *needfree)
{
  *needfree = false;
  bool gotnewline=(strrchr(msg,'\n')!=NULL);

  if (gotnewline && (printbufpos==0))
  {
     *needfree = true;
     return dupstr(msg); // have to dupe because we can't return the const char * parameter
  }

  size_t newlen=strlen(msg);

  if ((newlen>=printbufsize) && (printbufpos==0))
  {
     *needfree = true;
     return dupstr(msg); // have to dupe because we can't return the const char * parameter
  }

  size_t newtotallen=printbufpos+newlen;

  printbuf[printbufpos] = 0;
  if (newtotallen>=printbufsize)
  {
     char *newstr=malloc(newtotallen+1);
     newstr[0]=0;
     strcpy(newstr,printbuf);
     strcat(newstr,msg);
     *needfree = true;
     printbufpos=0;
     return newstr;
  }

  strcat(printbuf,msg);
  if (gotnewline)
  {
     printbufpos=0;
     return printbuf;
  }
  printbufpos+=newlen;
  return NULL;
}

int tgdll_print(const char *msg)
{
   if ((curlibctx==NULL) || (!curlibctx->printmessage_callback))
	  return printf("%s",msg);
   else
   {
      bool needfree=false;
      char *pr=printnow(msg,&needfree);
      if (pr)
      {
   	     curlibctx->printmessage_callback(pr,0,curlibctx);
         if (needfree)
            free(pr);
      }
	  return (int)strlen(msg);
   }
}

int tgdll_printfree(char *msg)
{
  int res=tgdll_print(msg);
  sfree(msg);
  return res;
}

int tgdll_fprint(FILE *stream,const char *msg)
{
   if (!curlibctx->printmessage_callback || ((stream!=stdout) && (stream!=stderr)))
	  return fprintf(stream,"%s",msg);
   else
   {
      bool needfree=false;
      char *pr=printnow(msg,&needfree);
      if (pr)
      {
		 curlibctx->printmessage_callback(pr,stream==stderr ? 1 : 0,curlibctx);
		 if (needfree)
            free(pr);
      }
	  return (int)strlen(msg);
   }
}

int tgdll_fprintfree(FILE *stream,char *msg)
{
  int res=tgdll_fprint(stream,msg);
  sfree(msg);
  return res;
}

int tgdll_fflush(FILE *stream)
{
  if ((stream!=stdout) && (stream!=stdin) && (stream!=stderr))
	 return fflush(stream);
  else
     return 0;
}

size_t tgdll_fwrite(const void *ptr,size_t size,size_t count,FILE *stream)
{
  if ((stream==stdout) || (stream==stderr))
  {
	 // append zero to data
	 size_t total=count*size;
	 char *buf=malloc(total+1);

	 // if there is a zero in the data, it will just terminate the string
	 // don't care about that
	 strncpy(buf,ptr,total);
     buf[total]=0;

	 tgdll_fprint(stream,buf);
	 free(buf);
	 return total;
  }
  else
     return fwrite(ptr,size,count,stream);
}

void tgdll_assert(const char *msg,const char *filename,const int line)
{
  if (curlibctx->raise_exception_callback)
     curlibctx->raise_exception_callback(msg,filename,line,curlibctx);
  else
  {
     printf("%s",msg);
     exit(999);
  }
}

#ifdef DEBUG_MALLOC
#undef malloc
#undef free
#undef realloc
void *tgdllmalloc(size_t size)
{
  if ((curlibctx!=NULL) && (curlibctx->usememorycallbacks) && (curlibctx->malloc_callback != NULL))
  {
     void *result=curlibctx->malloc_callback(size);
     return result;
  }
  else
     return malloc(size);
}

void tgdllfree(void *ptr)
{
  if ((curlibctx!=NULL) && (curlibctx->usememorycallbacks) && (curlibctx->free_callback != NULL))
     curlibctx->free_callback(ptr);
  else
     free(ptr);
}

void *tgdllrealloc(void *ptr, size_t new_size)
{
  if ((curlibctx!=NULL) && (curlibctx->usememorycallbacks) && (curlibctx->realloc_callback != NULL))
  {
     void *result=curlibctx->realloc_callback(ptr,new_size);
     return result;
  }
  else
     return realloc(ptr,new_size);
}

void *tgdlldebugmalloc(size_t size,const char *filename,const int line)
{
  if ((curlibctx!=NULL) && (curlibctx->usememorycallbacks) && (curlibctx->malloc_callback != NULL))
  {
     void *result=curlibctx->debug_malloc_callback(size,filename,line);
     return result;
  }
  else
     return malloc(size);
}
void tgdlldebugfree(void *ptr,const char *filename,const int line)
{
  if ((curlibctx!=NULL) && (curlibctx->usememorycallbacks) && (curlibctx->free_callback != NULL))
     curlibctx->debug_free_callback(ptr,filename,line);
  else
     free(ptr);
}

void *tgdlldebugrealloc(void *ptr, size_t new_size,const char *filename,const int line)
{
  if ((curlibctx!=NULL) && (curlibctx->usememorycallbacks) && (curlibctx->realloc_callback != NULL))
  {
     void *result=curlibctx->debug_realloc_callback(ptr,new_size,filename,line);
     return result;
  }
  else
     return realloc(ptr,new_size);
}


#endif

#ifdef _WINDOWS
BOOL WINAPI DllMain( HMODULE hModule,
					 DWORD  fdwReason,
                     LPVOID lpReserved)
{
    //printf("DllMain start, Reason: %ud.\n",fdwReason);
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
    {
  	  //printf("DLL_PROCESS_ATTACH\n");
      init_thread_vars();
      break;
    }
    case DLL_THREAD_ATTACH:
    {
  	  //printf("DLL_THREAD_ATTACH\n");
      init_thread_vars();
      break;
    }
    case DLL_THREAD_DETACH:
    {
  	  //printf("DLL_THREAD_DETACH\n");
      curlibctx=NULL;
      free_thread_vars();
      break;
    }
    case DLL_PROCESS_DETACH:
  	  //printf("PROCESS_DETACH\n");
      curlibctx=NULL; // surely no longer valid!
	  free_thread_vars();
      sk_cleanup(true);
	  break;
	}
	return TRUE;
   //printf("DllMain END.\n");
}
#endif

