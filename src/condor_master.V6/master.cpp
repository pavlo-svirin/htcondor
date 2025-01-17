/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


#include "condor_common.h"
#include "condor_debug.h"
#include "condor_config.h"
#include "condor_uid.h"
#include "condor_string.h"
#include "basename.h"
#include "master.h"
#include "subsystem_info.h"
#include "condor_daemon_core.h"
#include "condor_collector.h"
#include "condor_attributes.h"
#include "condor_network.h"
#include "condor_adtypes.h"
#include "condor_io.h"
#include "directory.h"
#include "exit.h"
#include "string_list.h"
#include "get_daemon_name.h"
#include "daemon_types.h"
#include "daemon_list.h"
#include "strupr.h"
#include "condor_environ.h"
#include "store_cred.h"
#include "setenv.h"
#include "file_lock.h"
#include "shared_port_server.h"
#include "shared_port_endpoint.h"

#if defined(WANT_CONTRIB) && defined(WITH_MANAGEMENT)
#if defined(HAVE_DLOPEN) || defined(WIN32)
#include "MasterPlugin.h"
#endif
#if defined(WIN32)
extern int load_master_mgmt(void);
#endif
#endif

#include "systemd_manager.h"
#include <sstream>

#ifdef WIN32

#include "firewall.WINDOWS.h"

extern DWORD start_as_service();
extern void terminate(DWORD);
#endif


// local function prototypes
void	init_params();
void	init_daemon_list();
void	init_classad();
void	init_firewall_exceptions();
void	check_uid_for_privsep();
void	lock_or_except(const char * );
time_t 	GetTimeStamp(char* file);
int 	NewExecutable(char* file, time_t* tsp);
void	RestartMaster();
void	run_preen();
void	usage(const char* );
void	main_shutdown_graceful();
void	main_shutdown_normal(); // do graceful or peaceful depending on daemonCore state
void	main_shutdown_fast();
void	invalidate_ads();
void	main_config();
int	agent_starter(ReliSock *, Stream *);
int	handle_agent_fetch_log(ReliSock *);
int	admin_command_handler(Service *, int, Stream *);
int	ready_command_handler(Service *, int, Stream *);
int	handle_subsys_command(int, Stream *);
int     handle_shutdown_program( int cmd, Stream* stream );
int     set_shutdown_program( const char * name );
void	time_skip_handler(void * /*data*/, int delta);
void	restart_everyone();

extern "C" int	DoCleanup(int,int,const char*);

// Global variables
ClassAd	*ad = NULL;				// ClassAd to send to collector
int		MasterLockFD;
int		update_interval;
int		check_new_exec_interval;
int		preen_interval;
int		preen_pid = -1;
int		new_bin_delay;
StopStateT new_bin_restart_mode = GRACEFUL;
char	*MasterName = NULL;
char	*shutdown_program = NULL;

int		master_backoff_constant = 9;
int		master_backoff_ceiling = 3600;
float	master_backoff_factor = 2.0;		// exponential factor
int		master_recover_time = 300;			// recover factor

char	*FS_Preen = NULL;
int		NT_ServiceFlag = FALSE;		// TRUE if running on NT as an NT Service

int		shutdown_graceful_timeout;
int		shutdown_fast_timeout;

int		PublishObituaries;
int		Lines;
int		AllowAdminCommands = FALSE;
int		StartDaemons = TRUE;
int		GotDaemonsOff = FALSE;
int		MasterShuttingDown = FALSE;

const char	*default_daemon_list[] = {
	"MASTER",
	"STARTD",
	"SCHEDD",
	0};

// NOTE: When adding something here, also add it to the various condor_config
// examples in src/condor_examples
char	default_dc_daemon_list[] =
"MASTER, STARTD, SCHEDD, KBDD, COLLECTOR, NEGOTIATOR, EVENTD, "
"VIEW_SERVER, CONDOR_VIEW, VIEW_COLLECTOR, CREDD, HAD, "
"REPLICATION, JOB_ROUTER, ROOSTER, SHARED_PORT, "
"DEFRAG, GANGLIAD, ANNEXD";

// create an object of class daemons.
class Daemons daemons;

class SystemdNotifier : public Service {

public:
	SystemdNotifier() : m_watchdog_timer(-1) {}

	void
	config()
	{
		const condor_utils::SystemdManager & sd = condor_utils::SystemdManager::GetInstance();
		int watchdog_secs = sd.GetWatchdogUsecs();
		if ( watchdog_secs > 0 ) {
			watchdog_secs = watchdog_secs / 1e6 / 3;
			if (watchdog_secs <= 0) { watchdog_secs = 1; }
			if (watchdog_secs > 20) { watchdog_secs = 10; }
			Timeslice ts;
			ts.setDefaultInterval(watchdog_secs);
			m_watchdog_timer = daemonCore->Register_Timer(ts,
					static_cast<TimerHandlercpp>(&SystemdNotifier::status_handler),
					"systemd status updater", this);
			if (m_watchdog_timer < 0) {
				dprintf(D_ALWAYS, "Failed to register systemd update timer.\n");
			} else {
				dprintf(D_FULLDEBUG, "Set systemd to be notified once every %d seconds.\n", watchdog_secs);
			}
		} else {
			dprintf(D_FULLDEBUG, "Not setting systemd watchdog timer\n");
		}
	}

	void status_handler()
	{
		char *name;
		class daemon *daemon;

		daemons.ordered_daemon_names.rewind();
		std::stringstream ss;
		ss << "Problems: ";
		bool had_prior = false;
		bool missing_daemons = false;
		while( (name = daemons.ordered_daemon_names.next()) ) {
			daemon = daemons.FindDaemon( name );
			if (!daemon->pid)
			{
				if (had_prior) { ss << ", "; }
				else { had_prior = true; }
				missing_daemons = true;
				time_t starttime = daemon->GetNextRestart();
				if (starttime)
				{
					time_t secs_to_start = starttime-time(NULL);
					if (secs_to_start > 0)
					{ ss << name << "=RESTART in " << secs_to_start << "s"; }
					else
					{ ss << name << "=RESTARTNG"; }
				}
				else
				{ ss << name << "=STOPPED"; }
			}
		}
		std::string status = ss.str();

		const char * format_string = "STATUS=%s\nWATCHDOG=1";
		if (!missing_daemons)
		{
			format_string = "READY=1\nSTATUS=%s\nWATCHDOG=1";
			status = "All daemons are responding";
		}

		const condor_utils::SystemdManager &sd = condor_utils::SystemdManager::GetInstance();
		int result = sd.Notify(format_string, status.c_str());
		if (result == 0)
		{
			dprintf(D_ALWAYS, "systemd watchdog notification support not available.\n");
			daemonCore->Cancel_Timer(m_watchdog_timer);
			m_watchdog_timer = -1;
		}
	}

private:
	int m_watchdog_timer;
};
SystemdNotifier g_systemd_notifier;

// called at exit to deallocate stuff so that memory checking tools are
// happy and don't think we leaked any of this...
static void
cleanup_memory( void )
{
	if ( ad ) {
		delete ad;
		ad = NULL;
	}
	if ( MasterName ) {
		free( MasterName );
		MasterName = NULL;
	}
	if ( FS_Preen ) {
		free( FS_Preen );
		FS_Preen = NULL;
	}
}

#ifdef WIN32
// we can't use execl on Windows (read the docs), and anyway, we don't want to
// if we are running as a serices because we MUST call our terminate() function to exit
static void Win32RunShutdownProgram(char * shutdown_program)
{
	STARTUPINFO si;
	ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);

	HANDLE hLog = INVALID_HANDLE_VALUE;

	// Open a log file for the shutdown programs stdout and stderr
	std::string shutdown_log;
	if (param(shutdown_log, "MASTER_SHUTDOWN_LOG") &&  ! shutdown_log.empty()) {
		SECURITY_ATTRIBUTES sa;
		ZeroMemory(&sa, sizeof(sa));
		sa.bInheritHandle = TRUE;

		hLog = CreateFile(shutdown_log.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (INVALID_HANDLE_VALUE != hLog) { si.hStdError = si.hStdOutput = hLog; }

		si.dwFlags = STARTF_USESTDHANDLES;
	}

	// create the shutdown program, we create it suspended so that we can log success or failure
	// and then get a head start to exit before it actually begins running.  That still doesn't
	// guarantee that we have exited before it starts though...
	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));
	priv_state p = set_root_priv( );
	BOOL exec_status = CreateProcess( NULL, shutdown_program, NULL, NULL, TRUE, DETACHED_PROCESS | CREATE_SUSPENDED, NULL, NULL, &si, &pi );
	set_priv( p );

	if (INVALID_HANDLE_VALUE != hLog) {
		formatstr(shutdown_log, "==== Master (pid %d) shutting down. output of shutdown script '%s' (pid %d) follows ====\n",
			GetCurrentProcessId(), shutdown_program, pi.dwProcessId);
		DWORD dwWrote;
		WriteFile(hLog, shutdown_log.c_str(), (DWORD)shutdown_log.size(), &dwWrote, NULL);
		CloseHandle(hLog);
	}

	if ( ! exec_status ) {
		dprintf( D_ALWAYS, "**** CreateProcess(%s) FAILED %d %s\n",
					shutdown_program, GetLastError(), GetLastErrorString(GetLastError()) );
	} else {
		dprintf( D_ALWAYS, "**** CreateProcess(%s) SUCCEEDED pid=%lu\n", shutdown_program, pi.dwProcessId );
		ResumeThread(pi.hThread); // let the shutdown process start.
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
}
#endif


int
master_exit(int retval)
{
	// If a shutdown_program has not been setup via the admin
	// command-line tool, see if the condor_config specifies
	// a default shutdown program to use.
	if ( !shutdown_program ) {
		char *defshut = param("DEFAULT_MASTER_SHUTDOWN_SCRIPT");
		if (defshut) {
			set_shutdown_program(defshut);
			free(defshut);
		}
	}

	cleanup_memory();

#ifdef WIN32
	if ( NT_ServiceFlag == TRUE ) {
		// retval == 2 indicates that we never actually started
		// so we don't want to invoke the shutdown program in that case.
		if (retval != 2 && shutdown_program) {
			dprintf( D_ALWAYS,
						"**** %s (%s_%s) pid %lu EXECING SHUTDOWN PROGRAM %s\n",
						"condor_master", "condor", "MASTER", GetCurrentProcessId(), shutdown_program );
			Win32RunShutdownProgram(shutdown_program);
			shutdown_program = NULL; // in case terminate doesn't kill us, make sure we don't run the shutdown program more than once.
		}
		terminate(retval);
	}
#endif

#if defined(WANT_CONTRIB) && defined(WITH_MANAGEMENT)
#if defined(HAVE_DLOPEN) || defined(WIN32)
	MasterPluginManager::Shutdown();
#endif
#endif

		// If we're positive that we are going to shut down,
		// we should clean out the shared port directory if
		// we created it.
	std::string dirname;
	if ( SharedPortEndpoint::CreatedSharedPortDirectory() &&
		 SharedPortEndpoint::GetDaemonSocketDir(dirname) ) {

		TemporaryPrivSentry tps(PRIV_CONDOR);
		Directory d(dirname.c_str());
		d.Remove_Entire_Directory();
		if (-1 == rmdir(dirname.c_str())) {
			dprintf(D_ALWAYS, "ERROR: failed to remove shared port temporary directory: %s (errno=%d).\n", strerror(errno), errno);
		}
	}

	// Exit via specified shutdown_program UNLESS retval is 2, which
	// means the master never started and we are exiting via usage() message.
#ifdef WIN32
	if (retval != 2 && shutdown_program) {
		dprintf( D_ALWAYS,
					"**** %s (%s_%s) pid %lu RUNNING SHUTDOWN PROGRAM %s\n",
					"condor_master", "condor", "MASTER", GetCurrentProcessId(), shutdown_program );
		Win32RunShutdownProgram(shutdown_program);
		shutdown_program = NULL; // don't let DC_Exit run the shutdown program on Windows.
	}
#endif
	DC_Exit(retval, retval == 2 ? NULL : shutdown_program );

	return 1;	// just to satisfy vc++
}

void
usage( const char* name )
{
	dprintf( D_ALWAYS, "Usage: %s [-f] [-t] [-n name]\n", name );
	// Note: master_exit with value of 2 means do NOT run any shutdown_program
	master_exit( 2 );
}

int
DoCleanup(int,int,const char*)
{
	static int already_excepted = FALSE;

	// This function gets called as the last thing by EXCEPT().
	// At this point, the MASTER presumably has had an unrecoverable error
	// and is about to die.  We'll attempt to forcibly shutdown our
	// kiddies, and all their offspring before we fade away.  
	// Use a static flag to prevent infinite recursion in the case 
	// that there is another EXCEPT.

	// Once here, we are never returning to daemon core, so reapers, timers
	// etc. will never fire again, so we need to get everything we need
	// to do done now.

	if ( already_excepted == FALSE ) {
		already_excepted = TRUE;
		daemons.HardKillAllDaemons();
	}

	if ( NT_ServiceFlag == FALSE ) {
		return 1;	// this will return to the EXCEPT code, which will just exit
	} else {
		master_exit(1); // this does not return
		return 1;		// just to satisfy VC++
	}
}

#if defined( LINUX )
void do_linux_kernel_tuning() {
	std::string kernelTuningScript;
	if(! param( kernelTuningScript, "LINUX_KERNEL_TUNING_SCRIPT" )) {
		dprintf( D_FULLDEBUG, "Not tuning kernel parameters: LINUX_KERNEL_TUNING_SCRIPT not defined.\n" );
		return;
	}

	std::string kernelTuningLogFile;
	param( kernelTuningLogFile, "KERNEL_TUNING_LOG" );
	if( kernelTuningLogFile.empty() ) {
		kernelTuningLogFile = "/dev/null";
	}

	if(! can_switch_ids() ) {
		dprintf( D_FULLDEBUG, "Not tuning kernel parameters: can't switch IDs.\n" );
		return;
	}

	priv_state prev = set_root_priv();
	int fd = safe_open_no_create( kernelTuningScript.c_str(), O_RDONLY );
	set_priv( prev );
	if( -1 == fd ) {
		dprintf( D_FULLDEBUG, "Not tuning kernel parameters: can't open file '%s'\n", kernelTuningScript.c_str() );
		return;
	}

	struct stat stats;
	int rv = fstat( fd, & stats );
	if( rv != 0 ) {
		dprintf( D_FULLDEBUG, "Not tuning kernel parameters: can't stat file '%s'\n", kernelTuningScript.c_str() );
		close( fd );
		return;
	}
	if( stats.st_uid != 0 ) {
		dprintf( D_FULLDEBUG, "Not tuning kernel parameters: file '%s' must be owned by root.\n", kernelTuningScript.c_str() );
		close( fd );
		return;
	}
	if( (stats.st_mode & S_IWGRP) || (stats.st_mode & S_IWOTH) ) {
		dprintf( D_FULLDEBUG, "Not tuning kernel parameters: file '%s' is group or world -writeable\n", kernelTuningScript.c_str() );
		close( fd );
		return;
	}
	if(! (stats.st_mode & S_IXUSR)) {
		dprintf( D_FULLDEBUG, "Not tuning kernel parameters: file '%s' is not executable.\n", kernelTuningScript.c_str() );
		close( fd );
		return;
	}
	close( fd );

	// Redirect the script's output into the log directory, in case anybody
	// else cares about it.  This simplifies our code, since we don't.
	// Do NOT block waiting for the script to exit -- shoot it in the head
	// after twenty seconds and report an error.  Otherwise, use its exit
	// code appropriately (may report an error).
	pid_t childPID = fork();
	if( childPID == 0 ) {
		daemonCore->Forked_Child_Wants_Fast_Exit( true );
		dprintf_init_fork_child();

		priv_state prev = set_root_priv();

		int fd = open( "/dev/null", O_RDONLY );
		if( fd == -1 ) {
			dprintf( D_FULLDEBUG, "Not tuning kernel parameters: child failed to open /dev/null: %d.\n", errno );
			exit( 1 );
		}
		if( dup2( fd, 0 ) == -1 ) {
			dprintf( D_FULLDEBUG, "Not tuning kernel parameters: child failed to dup /dev/null: %d.\n", errno );
			exit( 1 );
		}
		fd = open( kernelTuningLogFile.c_str(), O_WRONLY | O_APPEND, 0644 );
		if ((fd < 0) && (errno == ENOENT)) {
			fd = open( kernelTuningLogFile.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644 );
		}
		if( fd == -1 ) {
			dprintf( D_FULLDEBUG, "Not tuning kernel parameters: child failed to open '%s': %d.\n", kernelTuningLogFile.c_str(), errno );
			exit( 1 );
		}
		if( dup2( fd, 1 ) == -1 ) {
			dprintf( D_FULLDEBUG, "Not tuning kernel parameters: child failed to dup log file '%s' to stdout: %d.\n", kernelTuningLogFile.c_str(), errno );
			exit( 1 );
		}
		if( dup2( fd, 2 ) == -1 ) {
			dprintf( D_FULLDEBUG, "Not tuning kernel parameters: child failed to dup log file '%s' to stderr: %d.\n", kernelTuningLogFile.c_str(), errno );
			exit( 1 );
		}

		execl( kernelTuningScript.c_str(), kernelTuningScript.c_str(), (char *) NULL );

		set_priv( prev );
		dprintf( D_ALWAYS, "Did not tune kernel paramters: execl() failed: %d.\n", errno );
		exit( 1 );
	} else {
		int status = 0;
		pid_t wait = 0;
		unsigned timer = 20;

		while( true ) {
			wait = waitpid( childPID, & status, WNOHANG );
			if( wait == childPID ) {
				if( WIFEXITED(status) && WEXITSTATUS(status) == 0 ) {
					dprintf( D_FULLDEBUG, "Kernel parameters tuned.\n" );
					return;
				} else {
					dprintf( D_FULLDEBUG, "Failed to tune kernel parameters, status code %d.\n", status );
					return;
				}
			} else if( wait == 0 ) {
				if( --timer > 0 ) {
					dprintf( D_FULLDEBUG, "Sleeping one second for kernel parameter tuning (pid %d).\n", childPID );
					sleep( 1 );
				} else {
					dprintf( D_FULLDEBUG, "Waited too long for kernel parameters to be tuned, hard-killing script.\n" );
					kill( childPID, SIGKILL );
					// Collect the zombie.
					wait = waitpid( childPID, &status, WNOHANG );
					ASSERT( wait != childPID );
					return;
				}
			} else {
				dprintf( D_FULLDEBUG, "waitpid() failed while waiting for kernel tuning (%d).  Killing child %d.\n", errno, childPID );
				kill( childPID, SIGKILL );
				// Try again to collect the zombie?
				waitpid( childPID, & status, WNOHANG );
				return;
			}
		}
	}

}
#endif

void
main_init( int argc, char* argv[] )
{
    extern int runfor;
	char	**ptr;

	if ( argc > 3 ) {
		usage( argv[0] );
	}
	
	int argc_count = 1;
	for( ptr=argv+1, argc_count = 1; argc_count<argc && *ptr; ptr++,argc_count++) {
		if( ptr[0][0] != '-' ) {
			usage( argv[0] );
		}
		switch( ptr[0][1] ) {
		case 'n':
			ptr++;
			if( !(ptr && *ptr) ) {
				EXCEPT( "-n requires another argument" );
			}
			if (MasterName) {
				free(MasterName);
			}
			MasterName = build_valid_daemon_name( *ptr );
			dprintf( D_ALWAYS, "Using name: %s\n", MasterName );
			break;
		default:
			usage( argv[0] );
		}
	}

#ifdef LINUX
	if ( can_switch_ids() &&
		 param_boolean("DISCARD_SESSION_KEYRING_ON_STARTUP",true) )
	{
#ifndef KEYCTL_JOIN_SESSION_KEYRING
  #define KEYCTL_JOIN_SESSION_KEYRING 1
#endif
		// Create a new empty session keyring, discarding the old one.
		// We do this in the master so a session keyring containing
		// root keys is not inadvertantly leaked to jobs
		// (esp scheduler universe jobs, since
		// KEYCTL_JOIN_SESSION_KEYRING will fail in RHEL6 if the calling
		// process has more than one thread, which is true when spawning
		// scheduler universe jobs as the schedd clones by default instead
		// of forking).  We also do this in the master instead
		// of daemonCore, since we don't want to create a brand new
		// session keyring for every shadow (each keyring uses up kernel resources).
		// If the syscall fails due to ENOSYS, don't worry about it,
		// since that simply says the keyring facility is not installed.
		if (syscall(__NR_keyctl, KEYCTL_JOIN_SESSION_KEYRING, "htcondor")==-1 &&
			errno != ENOSYS)
		{
			int saved_errno = errno;
#if defined(EDQUOT)
			if (saved_errno == EDQUOT) {
				dprintf(D_ALWAYS | D_FAILURE,
				   "Error during DISCARD_SESSION_KEYRING_ON_STARTUP, suggest "
				   "increasing /proc/sys/kernel/keys/root_maxkeys\n");
			}
#endif /* defined EDQUOT */

			if (saved_errno == EPERM) {
				dprintf(D_ALWAYS, "Permission denied error during DISCARD_SESSION_KEYRING_ON_STARTUP, continuing anyway\n");
			} else {
				EXCEPT("Failed DISCARD_SESSION_KEYRING_ON_STARTUP=True errno=%d",
						saved_errno);
			}
		}
	}
#endif /* defined LINUX */

    if (runfor != 0) {
        // We will construct an environment variable that 
        // tells the daemon what time it will be shut down. 
        // We'll give it an absolute time, though runfor is a 
        // relative time. This means that we don't have to update
        // the time each time we restart the daemon.
		MyString runfor_env;
		runfor_env.formatstr("%s=%ld", EnvGetName(ENV_DAEMON_DEATHTIME),
						   time(NULL) + (runfor * 60));
		SetEnv(runfor_env.Value());
    }

	daemons.SetDefaultReaper();
	
		// Grab all parameters needed by the master.
	init_params();
	validate_config(0, CONFIG_OPT_DEPRECATION_WARNINGS);
		// param() for DAEMON_LIST and initialize our daemons object.
	init_daemon_list();
	if ( daemons.SetupControllers() < 0 ) {
		EXCEPT( "Daemon initialization failed" );
	}
		// Lookup the paths to all the daemons we now care about.
	daemons.InitParams();
		// Initialize our classad;
	init_classad();  
		// Initialize the master entry in the daemons data structure.
	daemons.InitMaster();
		// Make sure if PrivSep is on we're not running as root
	check_uid_for_privsep();
		// open up the windows firewall 
	init_firewall_exceptions();

#if defined(WANT_CONTRIB) && defined(WITH_MANAGEMENT)
#if defined(HAVE_DLOPEN)
	MasterPluginManager::Load();
#elif defined(WIN32)
	load_master_mgmt();
#endif
	MasterPluginManager::Initialize();
#endif

	g_systemd_notifier.config();

		// Register admin commands
	daemonCore->Register_Command( RESTART, "RESTART",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( RESTART_PEACEFUL, "RESTART_PEACEFUL",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( DAEMONS_OFF, "DAEMONS_OFF",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( DAEMONS_OFF_FAST, "DAEMONS_OFF_FAST",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( DAEMONS_OFF_PEACEFUL, "DAEMONS_OFF_PEACEFUL",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( DAEMONS_ON, "DAEMONS_ON",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( MASTER_OFF, "MASTER_OFF",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( MASTER_OFF_FAST, "MASTER_OFF_FAST",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( DAEMON_ON, "DAEMON_ON",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( DAEMON_OFF, "DAEMON_OFF",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( DAEMON_OFF_FAST, "DAEMON_OFF_FAST",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( DAEMON_OFF_PEACEFUL, "DAEMON_OFF_PEACEFUL",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( CHILD_ON, "CHILD_ON",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( CHILD_OFF, "CHILD_OFF",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( CHILD_OFF_FAST, "CHILD_OFF_FAST",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	daemonCore->Register_Command( SET_SHUTDOWN_PROGRAM, "SET_SHUTDOWN_PROGRAM",
								  (CommandHandler)admin_command_handler, 
								  "admin_command_handler", 0, ADMINISTRATOR );
	// Command handler for stashing the pool password
	daemonCore->Register_Command( STORE_POOL_CRED, "STORE_POOL_CRED",
								(CommandHandler)&store_pool_cred_handler,
								"store_pool_cred_handler", NULL, CONFIG_PERM,
								D_FULLDEBUG );

	// Command handler for handling the ready state
	daemonCore->Register_CommandWithPayload( DC_SET_READY, "DC_SET_READY",
								  (CommandHandler)ready_command_handler,
								  "ready_command_handler", 0, WRITE );
	daemonCore->Register_CommandWithPayload( DC_QUERY_READY, "DC_QUERY_READY",
								  (CommandHandler)ready_command_handler,
								  "ready_command_handler", 0, READ );

	/*
	daemonCore->Register_Command( START_AGENT, "START_AGENT",
					  (CommandHandler)admin_command_handler, 
					  "admin_command_handler", 0, ADMINISTRATOR );
	*/

	daemonCore->RegisterTimeSkipCallback(time_skip_handler,0);

	_EXCEPT_Cleanup = DoCleanup;

#if !defined(WIN32)
	if( !dprintf_to_term_check() && param_boolean( "USE_PROCESS_GROUPS", true ) ) {
			// If we're not connected to a terminal, start our own
			// process group, unless the config file says not to.
		setsid();
	}
#endif

	if( StartDaemons ) {
		daemons.StartAllDaemons();
	}
	daemons.StartTimers();
}


int
ready_command_handler( Service*, int cmd, Stream* stm )
{
	ReliSock* stream = (ReliSock*)stm;
	ClassAd cmdAd;

	stream->decode();
	stream->timeout(15);
	if( !getClassAd(stream, cmdAd) || !stream->end_of_message()) {
		dprintf( D_ALWAYS, "Failed to receive ready command (%d) on TCP: aborting\n", cmd );
		return FALSE;
	}
	MyString daemon_name; // using MyString here because it will never return NULL
	cmdAd.LookupString("DaemonName", daemon_name);
	int daemon_pid = 0;
	cmdAd.LookupInteger("DaemonPID", daemon_pid);

	dprintf( D_FULLDEBUG, "Got ready command (%d) from %s pid=%d\n", cmd, daemon_name.c_str(), daemon_pid );

	switch (cmd) {
		case DC_SET_READY:
		{
			MyString state; // using MyString because its c_str() never faults or returns NULL
			cmdAd.LookupString("DaemonState", state);
			class daemon* daemon = daemons.FindDaemonByPID(daemon_pid);
			if ( ! daemon) {
				dprintf(D_ALWAYS, "Cant find daemon %s to set ready state '%s'\n", daemon_name.c_str(), state.c_str());
			} else {
				dprintf(D_ALWAYS, "Setting ready state '%s' for %s\n", state.c_str(), daemon_name.c_str());
				daemon->SetReadyState(state.c_str());
			}
		}
		return TRUE;

		case DC_QUERY_READY:
			return daemons.QueryReady(cmdAd, stm);
		return FALSE;

		default:
			EXCEPT("Unknown ready command (%d) in ready_command_handler", cmd);
	}

	return FALSE;
}

int
admin_command_handler( Service*, int cmd, Stream* stream )
{
	if(! AllowAdminCommands ) {
		dprintf( D_FULLDEBUG, 
				 "Got admin command (%d) while not allowed. Ignoring.\n",
				 cmd );
		return FALSE;
	}
	dprintf( D_FULLDEBUG, 
			 "Got admin command (%d) and allowing it.\n", cmd );
	switch( cmd ) {
	case RESTART:
		restart_everyone();
		return TRUE;
	case RESTART_PEACEFUL:
		daemons.immediate_restart = TRUE;
		daemons.RestartMasterPeaceful();
		return TRUE;
	case DAEMONS_ON:
		daemons.DaemonsOn();
		return TRUE;
	case DAEMONS_OFF:
		daemons.DaemonsOff();
		return TRUE;
	case DAEMONS_OFF_FAST:
		daemons.DaemonsOff( 1 );
		return TRUE;
	case DAEMONS_OFF_PEACEFUL:
		daemons.DaemonsOffPeaceful();
		return TRUE;
	case MASTER_OFF:
		daemonCore->Send_Signal( daemonCore->getpid(), SIGTERM );
		return TRUE;
	case MASTER_OFF_FAST:
		daemonCore->Send_Signal( daemonCore->getpid(), SIGQUIT );
		return TRUE;

	case SET_SHUTDOWN_PROGRAM:
		return handle_shutdown_program( cmd, stream );

			// These commands are special, since they all need to read
			// off the subsystem before they know what to do.  So, we
			// handle them with a special function.
	case DAEMON_ON:
	case DAEMON_OFF:
	case DAEMON_OFF_FAST:
	case DAEMON_OFF_PEACEFUL:
	case CHILD_ON:
	case CHILD_OFF:
	case CHILD_OFF_FAST:
		return handle_subsys_command( cmd, stream );

			// This function is also special, since it needs to read
			// off more info.  So, it is handled with a special function.
	case START_AGENT:
		if (daemonCore->Create_Thread(
				(ThreadStartFunc)&agent_starter, (void*)stream, 0 )) {
			return TRUE;
		} else {
			dprintf( D_ALWAYS, "ERROR: unable to create agent thread!\n");
			return FALSE;
		}
	default: 
		EXCEPT( "Unknown admin command (%d) in handle_admin_commands",
				cmd );
	}
	return FALSE;
}

int
agent_starter( ReliSock * s, Stream * )
{
	ReliSock* stream = (ReliSock*)s;
	char *subsys = NULL;

	stream->decode();
	if( ! stream->code(subsys) ||
		! stream->end_of_message() ) {
		dprintf( D_ALWAYS, "Can't read subsystem name\n" );
		free( subsys );
		return FALSE;
	}

	dprintf ( D_ALWAYS, "Starting agent '%s'\n", subsys );

	if( strcasecmp(subsys, "fetch_log") == 0 ) {
		free (subsys);
		return handle_agent_fetch_log( stream );
	}

	// default:

	free (subsys);
	dprintf( D_ALWAYS, "WARNING: unrecognized agent name\n" );
	return FALSE;
}

int
handle_agent_fetch_log (ReliSock* stream) {

	MyString daemon;
	int  res = FALSE;

	if( ! stream->code(daemon) ||
		! stream->end_of_message()) {
		dprintf( D_ALWAYS, "ERROR: fetch_log can't read daemon name\n" );
		return FALSE;
	}

	dprintf( D_ALWAYS, "INFO: daemon_name: %s\n", daemon.c_str() );

	// append _LOG to get the param name of the daemon log file
	daemon += "_LOG";

	dprintf( D_ALWAYS, "INFO: daemon_paramname: %s\n", daemon.c_str() );

	auto_free_ptr daemon_filename(param(daemon.c_str()));
	if ( daemon_filename ) {
		filesize_t	size;
		dprintf( D_ALWAYS, "INFO: daemon_filename: %s\n", daemon_filename.ptr() );
		stream->encode();
		res = (stream->put_file(&size, daemon_filename.ptr()) < 0);
	} else {
		dprintf( D_ALWAYS, "ERROR: fetch_log can't param for %s\n", daemon.c_str() );
	}

	return res;
}

int
handle_subsys_command( int cmd, Stream* stream )
{
	char* subsys = NULL;
	class daemon* daemon;

	stream->decode();
	if( ! stream->code(subsys) ) {
		dprintf( D_ALWAYS, "Can't read subsystem name\n" );
		free( subsys );
		return FALSE;
	}
	if( ! stream->end_of_message() ) {
		dprintf( D_ALWAYS, "Can't read end_of_message\n" );
		free( subsys );
		return FALSE;
	}
	subsys = strupr( subsys );
	if( !(daemon = daemons.FindDaemon(subsys)) ) {
		dprintf( D_ALWAYS, "Error: Can't find daemon of type \"%s\"\n", 
				 subsys );
		free( subsys );
		return FALSE;
	}
	dprintf( D_ALWAYS, "Handling daemon-specific command for \"%s\"\n", 
			 subsys );
	free( subsys );

	switch( cmd ) {
	case DAEMON_ON:
		daemon->Hold( false );
		return daemon->Start();
	case DAEMON_OFF:
		daemon->Hold( true );
		daemon->Stop();
		return TRUE;
	case DAEMON_OFF_FAST:
		daemon->Hold( true );
		daemon->StopFast();
		return TRUE;
	case DAEMON_OFF_PEACEFUL:
		daemon->Hold( true );
		daemon->StopPeaceful();
		return TRUE;
	case CHILD_ON:
		daemon->Hold( false, true );
		return daemon->Start( true );
	case CHILD_OFF:
		daemon->Hold( true, true );
		daemon->Stop( true );
		return TRUE;
	case CHILD_OFF_FAST:
		daemon->Hold( true, true );
		daemon->StopFast( true );
		return TRUE;
	default:
		EXCEPT( "Unknown command (%d) in handle_subsys_command", cmd );
	}
	return FALSE;
}


int
handle_shutdown_program( int cmd, Stream* stream )
{
	if ( cmd != SET_SHUTDOWN_PROGRAM ) {
		EXCEPT( "Unknown command (%d) in handle_shutdown_program", cmd );
	}

	MyString name;
	stream->decode();
	if( ! stream->code(name) ) {
		dprintf( D_ALWAYS, "Can't read program name in handle_shutdown_program\n" );
	}

	if ( name.IsEmpty() ) {
		return FALSE;
	}

	// Can we find it in the configuration?
	MyString	pname;
	pname =  "master_shutdown_";
	pname += name;
	char	*path = param( pname.Value() );
	if ( NULL == path ) {
		dprintf( D_ALWAYS, "No shutdown program defined for '%s'\n", name.c_str() );
		return FALSE;
	}

	int ret_val = set_shutdown_program( path );

	if (path) free(path);

	return ret_val;
}

int
set_shutdown_program(const char *path)
{
	// Try to access() it
# if defined(HAVE_ACCESS)
	priv_state	priv = set_root_priv();
	int status = access( path, X_OK );
	if ( status ) {
		dprintf( D_ALWAYS,
				 "WARNING: no execute access to shutdown program (%s)"
				 ": %d/%s\n", path, errno, strerror(errno) );
	}
	set_priv( priv );
# endif

	// OK, let's run with that
	if ( shutdown_program ) {
		free( shutdown_program );
	}
	shutdown_program = strdup(path);
	dprintf( D_ALWAYS,
			 "Shutdown program path set to %s\n", shutdown_program );
	return TRUE;
}

void
init_params()
{
	char	*tmp;
	static	int	master_name_in_config = 0;

	if( ! master_name_in_config ) {
			// First time, or we know it's not in the config file. 
		if( ! MasterName ) {
				// Not set on command line
			tmp = param( "MASTER_NAME" );
			if( tmp ) {
				MasterName = build_valid_daemon_name( tmp );
				master_name_in_config = 1;
				free( tmp );
			} 
		}
	} else {
		free( MasterName );
		tmp = param( "MASTER_NAME" );
		MasterName = build_valid_daemon_name( tmp );
		free( tmp );
	}
	if( MasterName ) {
		dprintf( D_FULLDEBUG, "Using name: %s\n", MasterName );
	}
			
	if (!param_boolean_crufty("START_MASTER", true)) {
			dprintf( D_ALWAYS, "START_MASTER was set to FALSE, shutting down.\n" );
			StartDaemons = FALSE;
			main_shutdown_graceful();
	}

		
	StartDaemons = TRUE;
	if (!param_boolean_crufty("START_DAEMONS", true)) {
			dprintf( D_ALWAYS, 
					 "START_DAEMONS flag was set to FALSE.  Not starting daemons.\n" );
			StartDaemons = FALSE;
	} 
		// If we were sent the daemons_off command, don't forget that
		// here. 
	if( GotDaemonsOff ) {
		StartDaemons = FALSE;
	}

	PublishObituaries = param_boolean_crufty("PUBLISH_OBITUARIES", true) ? TRUE : FALSE;

	Lines = param_integer("OBITUARY_LOG_LENGTH",20);

	master_backoff_constant = param_integer( "MASTER_BACKOFF_CONSTANT", 9, 1 );

	master_backoff_ceiling = param_integer( "MASTER_BACKOFF_CEILING", 3600,1 );

	master_backoff_factor = param_double( "MASTER_BACKOFF_FACTOR", 2.0, 0 );
	if( master_backoff_factor <= 0.0 ) {
    	master_backoff_factor = 2.0;
    }
	
	master_recover_time = param_integer( "MASTER_RECOVER_FACTOR", 300, 1 );

	update_interval = param_integer( "MASTER_UPDATE_INTERVAL", 5 * MINUTE, 1 );

	check_new_exec_interval = param_integer( "MASTER_CHECK_NEW_EXEC_INTERVAL", 5*MINUTE );

	new_bin_delay = param_integer( "MASTER_NEW_BINARY_DELAY", 2*MINUTE, 1 );

	new_bin_restart_mode = GRACEFUL;
	char * restart_mode = param("MASTER_NEW_BINARY_RESTART");
	if (restart_mode) {
#if 1
		StopStateT mode = StringToStopState(restart_mode);
#else
		static const struct {
			const char * text;
			StopStateT   mode;
			} modes[] = {
				{ "GRACEFUL", GRACEFUL },
				{ "PEACEFUL", PEACEFUL },
				{ "NEVER", NONE }, { "NONE", NONE }, { "NO", NONE },
			//	{ "FAST", FAST },
			//	{ "KILL", KILL },
			};
		StopStateT mode = (StopStateT)-1; // prime with -1 so we can detect bad input.
		for (int ii = 0; ii < (int)COUNTOF(modes); ++ii) {
			if (MATCH == strcasecmp(restart_mode, modes[ii].text)) {
				mode = modes[ii].mode;
				break;
			}
		}
#endif
		if (mode == (StopStateT)-1)	{
			dprintf(D_ALWAYS, "%s is not a valid value for MASTER_NEW_BINARY_RESTART. using GRACEFUL\n", restart_mode);
		}
		if (mode >= 0 && mode <= NONE)
			new_bin_restart_mode = mode;
		free(restart_mode);
	}

	preen_interval = param_integer( "PREEN_INTERVAL", 24*HOUR, 0 );
	if(preen_interval == 0) {
		EXCEPT("PREEN_INTERVAL in the condor configuration is too low (0).  Please set it to an integer in the range 1 to %d (default %d).  To disable condor_preen entirely, comment out PREEN.", INT_MAX, 24*HOUR);

	}

	shutdown_fast_timeout = param_integer( "SHUTDOWN_FAST_TIMEOUT", 5*MINUTE, 1 );

	shutdown_graceful_timeout = param_integer( "SHUTDOWN_GRACEFUL_TIMEOUT", 30*MINUTE, 1 );

	AllowAdminCommands = param_boolean( "ALLOW_ADMIN_COMMANDS", true );

	if( FS_Preen ) {
		free( FS_Preen );
	}
	FS_Preen = param( "PREEN" );
}


void
init_daemon_list()
{
	char	*daemon_name;
	StringList daemon_names, dc_daemon_names;

	daemons.ordered_daemon_names.clearAll();
	char* dc_daemon_list = param("DC_DAEMON_LIST");

	if( !dc_daemon_list ) {
		dc_daemon_names.initializeFromString(default_dc_daemon_list);
	}
	else {
		if ( *dc_daemon_list == '+' ) {
			MyString	dclist;
			dclist = default_dc_daemon_list;
			dclist += ", ";
			dclist += &dc_daemon_list[1];
			dc_daemon_names.initializeFromString( dclist.Value() );
		}
		else {
			dc_daemon_names.initializeFromString(dc_daemon_list);

			StringList default_list(default_dc_daemon_list);
			default_list.rewind();
			char *default_entry;
			int	missing = 0;
			while( (default_entry=default_list.next()) ) {
				if( !dc_daemon_names.contains_anycase(default_entry) ) {
					dprintf(D_ALWAYS,
							"WARNING: expected to find %s in"
							" DC_DAEMON_LIST, but it is not there.\n",
							default_entry );
					missing++;
				}
			}
			if ( missing ) {
				dprintf( D_ALWAYS,
						 "WARNING: "
						 "%d entries are missing from DC_DAEMON_LIST.  "
						 "Unless you know what you are doing, it "
						 "is best to leave DC_DAEMON_LIST undefined "
						 "so that the default settings are used, "
						 "or use the new 'DC_DAEMON_LIST = "
						 "+<list>' syntax.\n", missing );
			}
				
		}
		free(dc_daemon_list);
	}

		// Tolerate a trailing comma in the list
	dc_daemon_names.remove( "" );

	char* ha_list = param("MASTER_HA_LIST");
	if( ha_list ) {
			// Make MASTER_HA_LIST case insensitive by always converting
			// what we get to uppercase.
		StringList ha_names;
		ha_list = strupr( ha_list );
		ha_names.initializeFromString(ha_list);
			// Tolerate a trailing comma in the list
		ha_names.remove( "" );
		daemons.ordered_daemon_names.create_union( ha_names, false );

		ha_names.rewind();
		while( (daemon_name = ha_names.next()) ) {
			if(daemons.FindDaemon(daemon_name) == NULL) {
				if( dc_daemon_names.contains(daemon_name) ) {
					new class daemon(daemon_name, true, true );
				} else {
					new class daemon(daemon_name, false, true );
				}
			}
		}
	}

	char* daemon_list = param("DAEMON_LIST");
	if( daemon_list ) {
			// Make DAEMON_LIST case insensitive by always converting
			// what we get to uppercase.
		daemon_list = strupr( daemon_list );
		daemon_names.initializeFromString(daemon_list);
		free( daemon_list );
			// Tolerate a trailing comma in the list
		daemon_names.remove( "" );


			/*
			  Make sure that if COLLECTOR is in the list, put it at
			  the front...  unfortunately, our List template (what
			  StringList is defined in terms of) is amazingly broken
			  with regard to insert() and append().  :( insert()
			  usually means: insert *before* the current position.
			  however, if you're at the end, it inserts before the
			  last valid entry, instead of working like append() as
			  you'd expect.  OR, if you just called rewind() and
			  insert() (to insert at the begining, right?) it works
			  like append() and sticks it at the end!!  ARGH!!!  so,
			  we've got to call next() after rewind() so we really
			  insert it at the front of the list.  UGH!  EVIL!!!
			  Derek Wright <wright@cs.wisc.edu> 2004-12-23
			*/
		if( daemon_names.contains("COLLECTOR") ) {
			daemon_names.deleteCurrent();
			daemon_names.rewind();
			daemon_names.next();
			daemon_names.insert( "COLLECTOR" );
		}

			// start shared_port first for a cleaner startup
		if( daemon_names.contains("SHARED_PORT") ) {
			daemon_names.deleteCurrent();
			daemon_names.rewind();
			daemon_names.next();
			daemon_names.insert( "SHARED_PORT" );
		}
		else if( SharedPortEndpoint::UseSharedPort() ) {
			if( param_boolean("AUTO_INCLUDE_SHARED_PORT_IN_DAEMON_LIST",true) ) {
				dprintf(D_ALWAYS,"Adding SHARED_PORT to DAEMON_LIST, because USE_SHARED_PORT=true (to disable this, set AUTO_INCLUDE_SHARED_PORT_IN_DAEMON_LIST=False)\n");
				daemon_names.rewind();
				daemon_names.next();
				daemon_names.insert( "SHARED_PORT" );
			}
		}

		daemons.ordered_daemon_names.create_union( daemon_names, false );

		daemon_names.rewind();
		while( (daemon_name = daemon_names.next()) ) {
			if(daemons.FindDaemon(daemon_name) == NULL) {
				if( dc_daemon_names.contains(daemon_name) ) {
					new class daemon(daemon_name);
				} else {
					new class daemon(daemon_name, false);
				}
			}
		}
	} else {
		daemons.ordered_daemon_names.create_union( dc_daemon_names, false );
		for(int i = 0; default_daemon_list[i]; i++) {
			new class daemon(default_daemon_list[i]);
		}
	}

}


void
init_classad()
{
	if( ad ) delete( ad );
	ad = new ClassAd();

	SetMyTypeName(*ad, MASTER_ADTYPE);
	SetTargetTypeName(*ad, "");

	if (MasterName) {
		ad->Assign(ATTR_NAME, MasterName);
	} else {
		char* default_name = default_daemon_name();
		if( ! default_name ) {
			EXCEPT( "default_daemon_name() returned NULL" );
		}
		ad->Assign(ATTR_NAME, default_name);
		free(default_name);
	}

#if !defined(WIN32)
	ad->Assign(ATTR_REAL_UID, (int)getuid());
#endif
}

#ifndef WIN32
FileLock *MasterLock;
#endif

void
lock_or_except( const char* file_name )
{
#ifndef WIN32	// S_IRUSR and S_IWUSR don't exist on WIN32, and 
				// we don't need to worry about multiple masters anyway
				// because it's a service.
	MasterLockFD=_condor_open_lock_file(file_name,O_WRONLY|O_CREAT|O_APPEND,S_IRUSR|S_IWUSR);
	if( MasterLockFD < 0 ) {
		EXCEPT( "can't safe_open_wrapper(%s,O_WRONLY|O_CREAT|O_APPEND,S_IRUSR|S_IWUSR) - errno %i", 
			file_name, errno );
	}

	// This must be a global so that it doesn't go out of scope
	// cause the destructor releases the lock.
	MasterLock = new FileLock( MasterLockFD, NULL, file_name );
	MasterLock->setBlocking( FALSE );
	if( !MasterLock->obtain(WRITE_LOCK) ) {
		EXCEPT( "Can't get lock on \"%s\"", file_name );
	}
#endif
}


/*
 ** Re read the config file, and send all the daemons a signal telling
 ** them to do so also.
 */
void
main_config()
{
	StringList old_daemon_list;
	char *list = daemons.ordered_daemon_names.print_to_string();
	char *daemon_name;
	class daemon	*adaemon;

	if( list ) {
		old_daemon_list.initializeFromString(list);
		free(list);
	}

		// Re-read the config files and create a new classad
	init_classad(); 

		// Reset our config values
	init_params();
	validate_config(0, CONFIG_OPT_DEPRECATION_WARNINGS);

		// Reset the daemon list
	init_daemon_list();

		// Remove daemons that should no longer be running
	old_daemon_list.rewind();
	while( (daemon_name = old_daemon_list.next()) ) {
		if( !daemons.ordered_daemon_names.contains(daemon_name) ) {
			if( NULL != daemons.FindDaemon(daemon_name) ) {
				daemons.StopDaemon(daemon_name);
			}
		}
	}

		// Re-read the paths to our executables.  If any paths
		// changed, the daemons will be marked as having a new
		// executable.
	daemons.InitParams();

	if( StartDaemons ) {
			// Restart any daemons who's executables are new or ones
			// that the path to the executable has changed.  
		daemons.immediate_restart = TRUE;
		daemons.CheckForNewExecutable();
		daemons.immediate_restart = FALSE;
			// Tell all the daemons that are running to reconfig.
		daemons.ReconfigAllDaemons();

			// Setup and configure controllers for all daemons in
			// case the reconfig changed controller setup.  Start
			// any new daemons as well
		daemons.ordered_daemon_names.rewind();
		while( ( daemon_name = daemons.ordered_daemon_names.next() ) ) {
			adaemon = daemons.FindDaemon(daemon_name);
			if ( adaemon == NULL ) {
				dprintf( D_ALWAYS, "ERROR: Setup for daemon %s failed\n", daemon_name );
			}
			else if ( adaemon->SetupController() < 0 ) {
				dprintf( D_ALWAYS,
						"ERROR: Setup of controller for daemon %s failed\n",
						daemon_name );
				daemons.StopDaemon( daemon_name );
			}
			else if( !old_daemon_list.contains(daemon_name) ) {
				daemons.StartDaemonHere(adaemon);
			}

		}

	} else {
		daemons.DaemonsOff();
	}
    // Invalide session if necessary
    daemonCore->invalidateSessionCache();
		// Re-register our timers, since their intervals might have
		// changed.
	daemons.StartTimers();
	daemons.UpdateCollector();
}

/*
 ** Kill all daemons and go away.
 */
void
main_shutdown_fast()
{
	invalidate_ads();
	
	MasterShuttingDown = TRUE;
	daemons.SetAllGoneAction( MASTER_EXIT );

	if( daemons.NumberOfChildren() == 0 ) {
		daemons.AllDaemonsGone();
	}

	daemons.CancelRestartTimers();
	daemons.StopFastAllDaemons();
}

/*
 ** Callback from daemon-core kill all daemons and go away. 
 */
void
main_shutdown_normal()
{
	// if we are doing peaceful tell the children, and set a timer to do the real shutdown
	// so the children have a chance to notice the messages
	//
	bool fTimer = false;
	if (daemonCore->GetPeacefulShutdown()) {
		int timeout = 5;
		if (daemons.SetPeacefulShutdown(timeout) > 0) {
			int tid = daemonCore->Register_Timer(timeout+1, 0,
							(TimerHandler)main_shutdown_graceful,
							"main_shutdown_graceful");
			if (tid == -1)
				dprintf( D_ALWAYS, "ERROR! Can't register DaemonCore timer!\n" );
			else
				fTimer = true;
		}
	}

	if ( ! fTimer) {
		main_shutdown_graceful();
	}
}

/*
 ** Cause job(s) to vacate, kill all daemons and go away.
 */
void
main_shutdown_graceful()
{
	invalidate_ads();
	
	MasterShuttingDown = TRUE;
	daemons.SetAllGoneAction( MASTER_EXIT );

	if( daemons.NumberOfChildren() == 0 ) {
		daemons.AllDaemonsGone();
	}

	daemons.CancelRestartTimers();
	daemons.StopAllDaemons();
}

void
invalidate_ads() {
	ClassAd cmd_ad;
	SetMyTypeName( cmd_ad, QUERY_ADTYPE );
	SetTargetTypeName( cmd_ad, MASTER_ADTYPE );
	
	MyString line;
	std::string escaped_name;
	char* default_name = MasterName ? ::strdup(MasterName) : NULL;
	if(!default_name) {
		default_name = default_daemon_name();
	}
	
	QuoteAdStringValue( default_name, escaped_name );
	line.formatstr( "( TARGET.%s == %s )", ATTR_NAME, escaped_name.c_str() );
	cmd_ad.AssignExpr( ATTR_REQUIREMENTS, line.Value() );
	cmd_ad.Assign( ATTR_NAME, default_name );
	cmd_ad.Assign( ATTR_MY_ADDRESS, daemonCore->publicNetworkIpAddr());
	daemonCore->sendUpdates( INVALIDATE_MASTER_ADS, &cmd_ad, NULL, false );
	free( default_name );
}

static const struct {
	StopStateT state;
	const char * name;
} aStopStateNames[] = {
	{ PEACEFUL, "PEACEFUL" },
	{ GRACEFUL, "GRACEFUL" },
	{ FAST,     "FAST" },
	{ KILL,     "KILL" },
	{ NONE,     "NEVER" }, { NONE, "NONE" }, { NONE, "NO" },
};

const char * StopStateToString(StopStateT state)
{
	// start by assuming that the index into the names table matches the state
	if ((int)state < (int)COUNTOF(aStopStateNames) && state == aStopStateNames[state].state) {
		return aStopStateNames[state].name;
	}
	// if the names table isn't packed and sorted, the brute force search it.
	for (int ii = 0; ii < (int)COUNTOF(aStopStateNames); ++ii) {
		if (aStopStateNames[ii].state == state) {
			return aStopStateNames[ii].name;
		}
	}
	return "??";
}

StopStateT StringToStopState(const char * psz)
{
	StopStateT state = (StopStateT)-1; // prime with -1 so we can detect bad input.
	for (int ii = 0; ii < (int)COUNTOF(aStopStateNames); ++ii) {
		if (MATCH == strcasecmp(psz, aStopStateNames[ii].name)) {
			state = aStopStateNames[ii].state;
			break;
		}
	}
	return state;
}


time_t
GetTimeStamp(char* file)
{
#ifdef WIN32
	ULARGE_INTEGER nanos;
	HANDLE hfile = CreateFile(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hfile == INVALID_HANDLE_VALUE) {
		return (time_t)-1;
	} else {
		BOOL fGotTime = GetFileTime(hfile, NULL, NULL, (FILETIME*)&nanos);
		CloseHandle(hfile);
		if ( ! fGotTime) {
			return (time_t)-1;
		}
	}
	// Windows filetimes are in 100 nanosecond intervals since January 1, 1601 (UTC)
	// NOTE: FAT records times on-disk in localtime, so daylight savings time can end up changing what is reported
	// the good news is NTFS stores UTC, so it doesn't have that problem.
	ULONGLONG nt_sec = nanos.QuadPart / (10 * 1000 * 1000); // convert to seconds,
	time_t epoch_sec = nt_sec - 11644473600; // convert from Windows 1600 epoch, to unix 1970 epoch
	return epoch_sec;
#else
	struct stat sbuf;
	
	if( stat(file, &sbuf) < 0 ) {
		return( (time_t) -1 );
	}
	
	return( sbuf.st_mtime );
#endif
}


int
NewExecutable(char* file, time_t *tsp)
{
	time_t cts = *tsp;
	*tsp = GetTimeStamp(file);
	dprintf(D_FULLDEBUG, "Time stamp of running %s: %ld\n",
			file, cts);
	dprintf(D_FULLDEBUG, "GetTimeStamp returned: %ld\n",*tsp);

	if( *tsp == (time_t) -1 ) {
		/*
		 **	We could have been in the process of installing a new
		 **	version, and that's why the 'stat' failed.  Catch it
		 **  next time around.
		 */
		*tsp = cts;
		return( FALSE );
	}
	return( cts != *tsp );
}

void
run_preen()
{
	char *args=NULL;
	const char	*preen_base;
	ArgList arglist;
	MyString error_msg;

	dprintf(D_FULLDEBUG, "Entered run_preen.\n");
	if ( preen_pid > 0 ) {
		dprintf( D_ALWAYS, "WARNING: Preen is already running (pid %d)\n", preen_pid );
	}

	if( FS_Preen == NULL ) {
		return;
	}
	preen_base = condor_basename( FS_Preen );
	arglist.AppendArg(preen_base);

	args = param("PREEN_ARGS");
	if(!arglist.AppendArgsV1RawOrV2Quoted(args,&error_msg)) {
		EXCEPT("ERROR: failed to parse preen args: %s",error_msg.Value());
	}
	free(args);

	preen_pid = daemonCore->Create_Process(
					FS_Preen,		// program to exec
					arglist,   		// args
					PRIV_ROOT,		// privledge level
					1,				// which reaper ID to use; use default reaper
					FALSE );		// we do _not_ want this process to have a command port; PREEN is not a daemon core process
	dprintf( D_ALWAYS, "Preen pid is %d\n", preen_pid );
}


void
RestartMaster()
{
	daemons.RestartMaster();
}

void
main_pre_command_sock_init()
{
	/* Make sure we are the only copy of condor_master running */
	char*  p;
#ifndef WIN32
	MyString lock_file;

	// see if a file is given explicitly
	p = param ("MASTER_INSTANCE_LOCK");
	if (p) {
		lock_file = p;
		free (p);
	} else {
		// no filename given.  use $(LOCK)/InstanceLock.
		p = param ("LOCK");
		if (p) {
			lock_file = p;
			lock_file = lock_file + "/InstanceLock";
			free (p);
		} else {
			// no LOCK dir?  strange.  fall back to the
			// old behavior which is to lock the log file
			// itself.
			p = param("MASTER_LOG");
			if (p) {
				lock_file = p;
				free (p);
			} else {
				// i give up.  have a hardcoded default and like it. :)
				lock_file = "/tmp/InstanceLock";
			}
		}
	}
	dprintf (D_FULLDEBUG, "Attempting to lock %s.\n", lock_file.Value() );
	lock_or_except( lock_file.Value() );
	dprintf (D_FULLDEBUG, "Obtained lock on %s.\n", lock_file.Value() );
#endif

	// Do any kernel tuning we've been configured to do.
	if( param_boolean( "ENABLE_KERNEL_TUNING", false ) ) {
#ifdef LINUX
		do_linux_kernel_tuning();
#endif
	}

	// If using CREDENTIAL_DIRECTORY, blow away the CREDMON_COMPLETE file
	// to force the credmon to refresh everything and to prevent the schedd
	// from starting up until credentials are ready.
	p = param("SEC_CREDENTIAL_DIRECTORY");
	if(p) {
		MyString cred_file;
		formatstr( cred_file, "%s%cCREDMON_COMPLETE", p, DIR_DELIM_CHAR );
		dprintf(D_SECURITY, "CREDMON: unlinking %s.", cred_file.Value());
		unlink(cred_file.Value());
	}
	free(p);

 	// in case a shared port address file got left behind by an
 	// unclean shutdown, clean it up now before we create our
 	// command socket to avoid confusion
	// Do so unconditionally, because the master will decide later (when
	// it's ready to start daemons) if it will be starting the shared
	// port daemon.
 	SharedPortServer::RemoveDeadAddressFile();

	// The master and its daemons may disagree on if they're using shared
	// port, so make sure everything's ready, just in case.
	//
	// FIXME: condor_preen doesn't look to know about "auto" directories.
	SharedPortEndpoint::InitializeDaemonSocketDir();
}

#ifdef WIN32
bool main_has_console() 
{
    DWORD displayMode;
    if (GetConsoleDisplayMode(&displayMode))
       return true;

        // if you need to debug service startup code
        // recompile with this code enabled, then attach the debugger
        // within 90 seconds of running "net start condor"
   #ifdef _DEBUG
    //for (int ii = 0; ii < 90*1000/500 && ! IsDebuggerPresent(); ++ii) { Sleep(500); } DebugBreak();
   #endif

    return false;
}
#endif


int
main( int argc, char **argv )
{
    // parse args to see if we have been asked to run as a service.
    // services are started without a console, so if we have one
    // we can't possibly run as a service.
    //
#ifdef WIN32
    bool has_console = main_has_console();
    bool is_daemon = dc_args_is_background(argc, argv);
#endif

	set_mySubSystem( "MASTER", SUBSYSTEM_TYPE_MASTER );

	dc_main_init = main_init;
	dc_main_config = main_config;
	dc_main_shutdown_fast = main_shutdown_fast;
	dc_main_shutdown_graceful = main_shutdown_normal;
	dc_main_pre_command_sock_init = main_pre_command_sock_init;

#ifdef WIN32
    // if we have been asked to start as a daemon, on Windows
    // first try and start as a service, if that doesn't work try
    // to start as a background process. Note that we don't return
    // from the call to start_as_service() if the service successfully
    // started - just like dc_main().
    //
    NT_ServiceFlag = FALSE;
    if (is_daemon) {
       NT_ServiceFlag = TRUE;
	   DWORD err = start_as_service();
       if (err == 0x666) {
          // 0x666 is a special error code that tells us 
          // the Service Control Manager didn't create this process
          // so we should go ahead run as normal background 'daemon'
          NT_ServiceFlag = FALSE;
       } else {
          return (int)err;
       }
    }
#endif

#ifdef LINUX
    // Check for necessary directories if we were started by the system
    if (getuid() == 0 && getppid() == 1) {
        // If the condor user is in LDAP, systemd will silently fail to create
        // these necessary directories at boot. The condor user and paths are
        // hard coded here, because they match the systemd configuration.
        struct stat sbuf;
        struct passwd *pwbuf = getpwnam("condor");
        if (pwbuf) {
            if (stat("/var/run/condor", &sbuf) != 0 && errno == ENOENT) {
                if (mkdir("/var/run/condor", 0775) == 0) {
                    if (chown("/var/run/condor", pwbuf->pw_uid, pwbuf->pw_gid)){}
                    if (chmod("/var/run/condor", 0775)){} // Override umask
                }
            }
            if (stat("/var/lock/condor", &sbuf) != 0 && errno == ENOENT) {
                if (mkdir("/var/lock/condor", 0775) == 0) {
                    if (chown("/var/lock/condor", pwbuf->pw_uid, pwbuf->pw_gid)){}
                    if (chmod("/var/lock/condor", 0775)){} // Override umask
                }
            }
        }
    }
#endif

	return dc_main( argc, argv );
}

void init_firewall_exceptions() {
#ifdef WIN32

	bool add_exception;
	char *master_image_path, *schedd_image_path, *startd_image_path,
		 *dagman_image_path, *negotiator_image_path, *collector_image_path, 
		 *starter_image_path, *shadow_image_path, *gridmanager_image_path, 
		 *gahp_image_path, *gahp_worker_image_path, *credd_image_path, 
		 *vmgahp_image_path, *kbdd_image_path, *bin_path;
	const char* dagman_exe = "condor_dagman.exe";

	WindowsFirewallHelper wfh;
	
	add_exception = param_boolean("ADD_WINDOWS_FIREWALL_EXCEPTION", NT_ServiceFlag);

	if ( add_exception == false ) {
		dprintf(D_FULLDEBUG, "ADD_WINDOWS_FIREWALL_EXCEPTION is false, skipping firewall configuration\n");
		return;
	}
	dprintf(D_ALWAYS, "Adding/Checking Windows firewall exceptions for all daemons\n");

	// We use getExecPath() here instead of param() since it's
	// possible the the Windows Service Control Manager
	// (SCM) points to one location for the master (which
	// is exec'd), while MASTER points to something else
	// (and ignored).
	
	master_image_path = getExecPath();
	if ( !master_image_path ) {	
		dprintf(D_ALWAYS, 
				"WARNING: Failed to get condor_master image path.\n"
				"Condor will not be excepted from the Windows firewall.\n");
		return;
	}

	// We want to add exceptions for the SCHEDD and the STARTD
	// so that (1) shadows can accept incoming connections on their 
	// command port and (2) so starters can do the same.

	schedd_image_path = param("SCHEDD");
	startd_image_path = param("STARTD");

	// And add exceptions for all the other daemons, since they very well
	// may need to open a listen port for mechanisms like CCB, or HTTPS
	negotiator_image_path = param("NEGOTIATOR");
	collector_image_path = param("COLLECTOR");
	starter_image_path = param("STARTER");
	shadow_image_path = param("SHADOW");
	gridmanager_image_path = param("GRIDMANAGER");
	gahp_image_path = param("CONDOR_GAHP");
	gahp_worker_image_path = param("CONDOR_GAHP_WORKER");
	credd_image_path = param("CREDD");
	kbdd_image_path = param("KBDD");
	vmgahp_image_path = param("VM_GAHP_SERVER");
	
	// We also want to add exceptions for the DAGMan we ship
	// with Condor:

	dagman_image_path = NULL; // make sure it's initialized.
	bin_path = param ( "BIN" );
	if ( bin_path ) {
		dagman_image_path = (char*) malloc (
			strlen ( bin_path ) + strlen ( dagman_exe ) + 2 );
		if ( dagman_image_path ) {
			sprintf ( dagman_image_path, "%s\\%s", bin_path, dagman_exe );
		}
		free ( bin_path );
	}

	// Insert the master
	if ( !SUCCEEDED(wfh.addTrusted(master_image_path)) ) {
		dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				master_image_path);
	}

	// Insert daemons needed on a central manager
	if ( (daemons.FindDaemon("NEGOTIATOR") != NULL) && negotiator_image_path ) {
		if ( !SUCCEEDED(wfh.addTrusted(negotiator_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				negotiator_image_path);
		}
	}
	if ( (daemons.FindDaemon("COLLECTOR") != NULL) && collector_image_path ) {
		if ( !SUCCEEDED(wfh.addTrusted(collector_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				collector_image_path);
		}
	}

	// Insert daemons needed on a submit node
	if ( (daemons.FindDaemon("SCHEDD") != NULL) && schedd_image_path ) {
		// put in schedd
		if ( !SUCCEEDED(wfh.addTrusted(schedd_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				schedd_image_path);
		}
		// put in shadow
		if ( shadow_image_path && !SUCCEEDED(wfh.addTrusted(shadow_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				shadow_image_path);
		}
		// put in gridmanager
		if ( gridmanager_image_path && !SUCCEEDED(wfh.addTrusted(gridmanager_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				gridmanager_image_path);
		}
		// put in condor gahp
		if ( gahp_image_path && !SUCCEEDED(wfh.addTrusted(gahp_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				gahp_image_path);
		}
		// put in condor worker gahp
		if ( gahp_worker_image_path && !SUCCEEDED(wfh.addTrusted(gahp_worker_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				gahp_worker_image_path);
		}
	}

	// Insert daemons needed on a execute node.
	// Note we include the starter and friends seperately, since the
	// starter could run on either execute or submit nodes (think 
	// local universe jobs).
	if ( (daemons.FindDaemon("STARTD") != NULL) && startd_image_path ) {
		if ( !SUCCEEDED(wfh.addTrusted(startd_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				startd_image_path);
		}
		if ( !SUCCEEDED(wfh.addTrusted(kbdd_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				kbdd_image_path);
		}
	}

	if ( starter_image_path ) {
		if ( !SUCCEEDED(wfh.addTrusted(starter_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				starter_image_path);
		}
	}

	if ( (daemons.FindDaemon("CREDD") != NULL) && credd_image_path ) {
		if ( !SUCCEEDED(wfh.addTrusted(credd_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				credd_image_path);
		}
	}

	if ( vmgahp_image_path ) {
		if ( !SUCCEEDED(wfh.addTrusted(vmgahp_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to the "
				"windows firewall exception list.\n",
				vmgahp_image_path);
		}
	}

	if ( dagman_image_path ) {
		if ( !SUCCEEDED(wfh.addTrusted (dagman_image_path)) ) {
			dprintf(D_FULLDEBUG, "WinFirewall: unable to add %s to "
				"the windows firewall exception list.\n",
				dagman_image_path);
		}
	}

	if ( master_image_path ) { free(master_image_path); }
	if ( schedd_image_path ) { free(schedd_image_path); }
	if ( startd_image_path ) { free(startd_image_path); }
	if ( dagman_image_path ) { free(dagman_image_path); }
	if ( negotiator_image_path ) { free(negotiator_image_path); }
	if ( collector_image_path ) { free(collector_image_path); }
	if ( shadow_image_path ) { free(shadow_image_path); }
	if ( gridmanager_image_path ) { free(gridmanager_image_path); }
	if ( gahp_image_path ) { free(gahp_image_path); }	
	if ( credd_image_path ) { free(credd_image_path); }	
	if ( vmgahp_image_path ) { free(vmgahp_image_path); }
	if ( kbdd_image_path ) { free(kbdd_image_path); }
#endif
}

void
check_uid_for_privsep()
{
#if !defined(WIN32)
	if (param_boolean("PRIVSEP_ENABLED", false) && (getuid() == 0)) {
		uid_t condor_uid = get_condor_uid();
		if (condor_uid == 0) {
			EXCEPT("PRIVSEP_ENABLED set, but current UID is 0 "
			           "and condor UID is also set to root");
		}
		dprintf(D_ALWAYS,
		        "PRIVSEP_ENABLED set, but UID is 0; "
		            "will drop to UID %u and restart\n",
		        (unsigned)condor_uid);
		daemons.CleanupBeforeRestart();
		set_condor_priv_final();
		daemons.ExecMaster();
		EXCEPT("attempt to restart (via exec) failed (%s)",
		       strerror(errno));
	}
#endif
}

void restart_everyone() {
		daemons.immediate_restart = TRUE;
		daemons.RestartMaster();
}

	// We could care less about our arguments.
void time_skip_handler(void * /*data*/, int delta)
{
	dprintf(D_ALWAYS, "The system clocked jumped %d seconds unexpectedly.  Restarting all daemons\n", delta);
	restart_everyone();
}
