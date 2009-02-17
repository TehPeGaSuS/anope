/* Services -- main source file.
 *
 * (C) 2003-2009 Anope Team
 * Contact us at team@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, write to the
 * Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id$
 *
 */

#include "services.h"
#include "timeout.h"
#include "version.h"
#include "datafiles.h"
#include "modules.h"

// getrlimit.
#ifndef _WIN32
# include <sys/time.h>
# include <sys/resource.h>
#endif


/******** Global variables! ********/

/* Command-line options: (note that configuration variables are in config.c) */
std::string services_dir;	  /* -dir dirname */
const char *log_filename = LOG_FILENAME;	  /* -log filename */
int debug = 0;				  /* -debug */
int readonly = 0;			   /* -readonly */
int logchan = 0;				/* -logchan */
int nofork = 0;				 /* -nofork */
int forceload = 0;			  /* -forceload */
int nothird = 0;				/* -nothrid */
int noexpire = 0;			   /* -noexpire */
int protocoldebug = 0;		  /* -protocoldebug */

std::string binary_dir; /* Used to store base path for Anope */
#ifdef _WIN32
#include <process.h>
#define execve _execve
#endif

/* Set to 1 if we are to quit */
int quitting = 0;

/* Set to 1 if we are to quit after saving databases */
int delayed_quit = 0;

/* Contains a message as to why services is terminating */
const char *quitmsg = NULL;

/* Input buffer - global, so we can dump it if something goes wrong */
char inbuf[BUFSIZE];

/* Socket for talking to server */
int servsock = -1;

/* Should we update the databases now? */
int save_data = 0;

/* At what time were we started? */
time_t start_time;

/* Parameters and environment */
char **my_av, **my_envp;

/* Moved here from version.h */
const char version_number[] = VERSION_STRING;
const char version_number_dotted[] = VERSION_STRING_DOTTED;
const char version_build[] =
	"build #" BUILD ", compiled " __DATE__ " " __TIME__;
/* the space is needed cause if you build with nothing it will complain */
const char version_flags[] = " " VER_OS VER_MYSQL VER_MODULE;

extern char *mod_current_buffer;

/******** Local variables! ********/

/* Set to 1 after we've set everything up */
static int started = 0;

/*************************************************************************/

/* Run expiration routines */

extern void expire_all()
{
	if (noexpire || readonly)
	{
		// Definitely *do not* want.
		return;
	}

	send_event(EVENT_DB_EXPIRE, 1, EVENT_START);
	if (debug)
		alog("debug: Running expire routines");
	expire_nicks();
	expire_chans();
	expire_requests();
	expire_akills();
	if (ircd->sgline) {
		expire_sglines();
	}
	if (ircd->sqline) {
		expire_sqlines();
	}
	if (ircd->szline) {
		expire_szlines();
	}
	expire_exceptions();
	send_event(EVENT_DB_EXPIRE, 1, EVENT_STOP);
}

/*************************************************************************/

void save_databases()
{
	send_event(EVENT_DB_SAVING, 1, EVENT_START);
	if (debug)
		alog("debug: Saving FFF databases");
	backup_databases();
	save_ns_dbase();
	if (PreNickDBName) {
		save_ns_req_dbase();
	}
	save_cs_dbase();
	if (s_BotServ) {
		save_bs_dbase();
	}
	if (s_HostServ) {
		save_hs_dbase();
	}
	save_os_dbase();
	save_news();
	save_exceptions();
	send_event(EVENT_DB_SAVING, 1, EVENT_STOP);
}

/*************************************************************************/

/* Restarts services */

static void services_restart()
{
	alog("Restarting");
	send_event(EVENT_RESTART, 1, EVENT_START);
	if (!quitmsg)
		quitmsg = "Restarting";
	ircdproto->SendSquit(ServerName, quitmsg);
	disconn(servsock);
	close_log();
	/* First don't unload protocol module, then do so */
	modules_unload_all(true, false);
	modules_unload_all(true, true);
	chdir(binary_dir.c_str());
	execve(SERVICES_BIN, my_av, my_envp);
	if (!readonly) {
		open_log();
		log_perror("Restart failed");
		close_log();
	}
}

/*************************************************************************/
/**
 * Added to allow do_restart from operserv access to the static functions without making them
 * fair game to every other function - not exactly ideal :|
 **/
void do_restart_services()
{
	if (!readonly) {
		expire_all();
		save_databases();
	}
	services_restart();
	exit(1);
}

/*************************************************************************/

/* Terminates services */

static void services_shutdown()
{
	User *u, *next;

	send_event(EVENT_SHUTDOWN, 1, EVENT_START);

	if (!quitmsg)
		quitmsg = "Terminating, reason unknown";
	alog("%s", quitmsg);
	if (started) {
		ircdproto->SendSquit(ServerName, quitmsg);
		if (uplink)
			delete [] uplink;
		if (mod_current_buffer)
			delete [] mod_current_buffer;
		if (ircd->chanmodes) {
			delete [] ircd->chanmodes;
		}
		u = firstuser();
		while (u) {
			next = nextuser();
			delete u;
			u = next;
		}
	}
	send_event(EVENT_SHUTDOWN, 1, EVENT_STOP);
	disconn(servsock);
	/* First don't unload protocol module, then do so */
	modules_unload_all(true, false);
	modules_unload_all(true, true);
	/* just in case they weren't all removed at least run once */
	ModuleRunTimeDirCleanUp();
}

/*************************************************************************/

/* If we get a weird signal, come here. */

void sighandler(int signum)
{
	/* We set the quit message to something default, just to be sure it is
	 * always set when we need it. It seems some signals slip through to the
	 * QUIT code without having a valid quitmsg. -GD
	 */
	if (!quitmsg)
		quitmsg = "Services terminating via a signal.";

	if (started)
	{
#ifndef _WIN32
		if (signum == SIGHUP)
		{
			alog("Received SIGHUP: Saving Databases & Rehash Configuration");

			expire_all();
			save_databases();

			if (!read_config(1))
			{
				quitmsg = "Error Reading Configuration File (Received SIGHUP)";
				quitting = 1;
			}

			FOREACH_MOD(I_OnReload, OnReload(true));
			return;

		} else
#endif
		if (signum == SIGTERM)
		{
			signal(SIGTERM, SIG_IGN);
#ifndef _WIN32
			signal(SIGHUP, SIG_IGN);
#endif

			alog("Received SIGTERM, exiting.");

			expire_all();
			save_databases();
			quitmsg = "Shutting down on SIGTERM";
			services_shutdown();
			exit(0);
		}
		else if (signum == SIGINT)
		{
			if (nofork)
			{
				signal(SIGINT, SIG_IGN);
				alog("Received SIGINT, exiting.");
				expire_all();
				save_databases();
				quitmsg = "Shutting down on SIGINT";
				services_shutdown();
				exit(0);
			}
		}
	}


	/* Should we send the signum here as well? -GD */
	send_event(EVENT_SIGNAL, 1, quitmsg);

	if (started)
	{
		services_shutdown();
		exit(0);
	}
	else
	{
		if (isatty(2))
		{
			fprintf(stderr, "%s\n", quitmsg);
		}
		else
		{
			alog("%s", quitmsg);
		}

		exit(1);
	}
}

/*************************************************************************/

/** The following comes from InspIRCd to get the full path of the Anope executable
 */
std::string GetFullProgDir(char *argv0)
{
	char buffer[PATH_MAX];
#ifdef _WIN32
	/* Windows has specific API calls to get the EXE path that never fail.
	 * For once, Windows has something of use, compared to the POSIX code
	 * for this, this is positively neato.
	 */
	if (GetModuleFileName(NULL, buffer, PATH_MAX))
	{
		std::string fullpath = buffer;
		std::string::size_type n = fullpath.rfind("\\" SERVICES_BIN);
		return std::string(fullpath, 0, n);
	}
#else
	// Get the current working directory
	if (getcwd(buffer, PATH_MAX))
	{
		std::string remainder = argv0;

		/* Does argv[0] start with /? If so, it's a full path, use it */
		if (remainder[0] == '/')
		{
			std::string::size_type n = remainder.rfind("/" SERVICES_BIN);
			return std::string(remainder, 0, n);
		}

		std::string fullpath = static_cast<std::string>(buffer) + "/" + remainder;
		std::string::size_type n = fullpath.rfind("/" SERVICES_BIN);
		return std::string(fullpath, 0, n);
	}
#endif
	return "/";
}

/*************************************************************************/

/* Main routine.  (What does it look like? :-) ) */

int main(int ac, char **av, char **envp)
{
	volatile time_t last_update;		/* When did we last update the databases? */
	volatile time_t last_expire;		/* When did we last expire nicks/channels? */
	volatile time_t last_check; /* When did we last check timeouts? */
	volatile time_t last_DefCon;		/* When was DefCon last checked? */

	int i;
	char *progname;

	my_av = av;
	my_envp = envp;

#ifndef _WIN32
	/* If we're root, issue a warning now */
	if ((getuid() == 0) && (getgid() == 0)) {
		fprintf(stderr,
				"WARNING: You are currently running Anope as the root superuser. Anope does not\n");
		fprintf(stderr,
				"	require root privileges to run, and it is discouraged that you run Anope\n");
		fprintf(stderr, "	as the root superuser.\n");
	}
#endif

	binary_dir = GetFullProgDir(av[0]);
	services_dir = binary_dir + "/../data";

	/* General initialization first */
	if ((i = init_primary(ac, av)) != 0)
		return i;

	/* Find program name. */
	if ((progname = strrchr(av[0], '/')) != NULL)
		progname++;
	else
		progname = av[0];

	/* Initialization stuff. */
	if ((i = init_secondary(ac, av)) != 0)
		return i;


	/* We have a line left over from earlier, so process it first. */
	process();

	/* Set up timers. */
	last_update = time(NULL);
	last_expire = time(NULL);
	last_check = time(NULL);
	last_DefCon = time(NULL);

	started = 1;

#ifndef _WIN32
	if (DumpCore)
	{
		rlimit rl;
		if (getrlimit(RLIMIT_CORE, &rl) == -1)
		{
			alog("Failed to getrlimit()!");
		}
		else
		{
			rl.rlim_cur = rl.rlim_max;
			if (setrlimit(RLIMIT_CORE, &rl) == -1)
			{
				alog("setrlimit() failed, cannot increase coredump size");
			}
		}
	}
#endif

	/*** Main loop. ***/

	while (!quitting) {
		time_t t = time(NULL);

		if (debug >= 2)
			alog("debug: Top of main loop");

		// Never fear. noexpire/readonly are checked in expire_all().
		if (save_data || t - last_expire >= ExpireTimeout)
		{
			expire_all();
			last_expire = t;
		}

		if (!readonly && (save_data || t - last_update >= UpdateTimeout)) {
			if (delayed_quit)
				ircdproto->SendGlobops(NULL,
								 "Updating databases on shutdown, please wait.");

			save_databases();

			if (save_data < 0)
				break;		  /* out of main loop */

			save_data = 0;
			last_update = t;
		}

		if ((DefConTimeOut) && (t - last_DefCon >= DefConTimeOut)) {
			resetDefCon(5);
			last_DefCon = t;
		}

		if (delayed_quit)
			break;

		ModuleManager::RunCallbacks();

		if (t - last_check >= TimeoutCheck) {
			check_timeouts();
			last_check = t;
		}

		/* this is a nasty nasty typecast. we need to rewrite the
		   socket stuff -Certus */
		i = static_cast<int>(reinterpret_cast<long>(sgets2(inbuf, sizeof(inbuf), servsock)));
		if ((i > 0) || (i < (-1))) {
			process();
		} else if (i == 0) {
			int errno_save = errno;
			quitmsg = new char[BUFSIZE];
			if (quitmsg) {
		// Naughty, but oh well. :)
				snprintf(const_cast<char *>(quitmsg), BUFSIZE,
						 "Read error from server: %s (error num: %d)",
						 strerror(errno_save), errno_save);
			} else {
				quitmsg = "Read error from server";
			}
			quitting = 1;

			/* Save the databases */
			if (!readonly)
				save_databases();
		}
	}


	/* Check for restart instead of exit */
	if (save_data == -2) {
#ifdef SERVICES_BIN
		alog("Restarting");
		if (!quitmsg)
			quitmsg = "Restarting";
		ircdproto->SendSquit(ServerName, quitmsg);
		disconn(servsock);
		close_log();
		chdir(binary_dir.c_str());
		execve(SERVICES_BIN, av, envp);
		if (!readonly) {
			open_log();
			log_perror("Restart failed");
			close_log();
		}
		return 1;
#else
		quitmsg =
			"Restart attempt failed--SERVICES_BIN not defined (rerun configure)";
#endif
	}

	/* Disconnect and exit */
	services_shutdown();

	return 0;
}

