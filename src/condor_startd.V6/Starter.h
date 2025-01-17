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

/*
  This file defines the Starter class, used by the startd to keep
  track of a resource's starter process.  

  Written 10/6/97 by Derek Wright <wright@cs.wisc.edu>
*/

#ifndef _CONDOR_STARTD_STARTER_H
#define _CONDOR_STARTD_STARTER_H

#include "../condor_procapi/procapi.h"
#include "../condor_procd/proc_family_io.h"

class Claim;

class Starter : public Service
{
public:
	Starter();
	Starter( const Starter& s );
	~Starter();


	void	dprintf( int, const char* ... );
	void	set_dprintf_prefix(const char * prefix) { if (prefix) { s_dpf = prefix; } else { s_dpf.clear(); } }

	char*	path() {return s_path;};
	time_t	birthdate( void ) {return s_birthdate;};
	bool	kill(int);
	bool	killpg(int);
	void	killkids(int);
	void	exited(Claim *, int status);
	int 	spawn(Claim *, time_t now, Stream* s );
	pid_t	pid() {return s_pid;};
	bool	is_dc() {return s_is_dc;};
	bool	active();
	const ProcFamilyUsage & updateUsage(void);


	void	setReaperID( int reaper_id ) { s_reaper_id = reaper_id; };
	bool    notYetReaped() { return (s_pid != 0) && ! s_was_reaped; }
	void    setOrphanedJob(ClassAd * job);

	void    setExecuteDir( char const * dir ) { s_execute_dir = dir; }
		// returns NULL if no execute directory set, o.w. returns the value
		// of EXECUTE that is passed to the starter
	char const *executeDir();
	char const *encryptedExecuteDir();

	bool	killHard( int timeout );
	bool	killSoft( int timeout, bool state_change = false );
	bool	suspend( void );
	bool	resume( void );

	bool	holdJob(char const *hold_reason,int hold_code,int hold_subcode,bool soft,int timeout);

		// Send SIGKILL to starter + process group (called by our kill
		// timer if we've been hardkilling too long).
	void	sigkillStarter( void );

		// Escalate to a fast shutdown of the job.
		// Called by our softkill timer
	void softkillTimeout( void );
	
	void	publish( ClassAd* ad, amask_t mask, StringList* list );

	bool	satisfies( ClassAd* job_ad, ClassAd* mach_ad );
	bool	provides( const char* ability );

	void	setAd( ClassAd* ad );
	void	setPath( const char* path );
	void	setIsDC( bool is_dc );

#if HAVE_BOINC
	bool	isBOINC( void ) { return s_is_boinc; };
	void	setIsBOINC( bool is_boinc ) { s_is_boinc = is_boinc; };
#endif /* HAVE_BOINC */

	void	setPorts( int, int );

	void	printInfo( int debug_level );

	char const*	getIpAddr( void );

	int receiveJobClassAdUpdate( Stream *stream );

	void holdJobCallback(DCMsgCallback *cb);

private:

		// methods
	bool	reallykill(int, int);
	int		execOldStarter( Claim * );
	int		execJobPipeStarter( Claim * );
	int		execDCStarter( Claim *, Stream* s );
		// claim is optional here, and may be NULL (e.g. boinc) but it may NOT be null when glexec is enabled. (sigh)
	int		execDCStarter( Claim *, ArgList const &args, Env const *env,
						   int std_fds[], Stream* s );
#if HAVE_BOINC
	int 	execBOINCStarter( Claim * );
#endif /* HAVE_BOINC */

#if !defined(WIN32)
		// support for spawning starter using glexec
	bool    prepareForGlexec( const ArgList&,
	                          const Env*,
	                          const int[3],
	                          ArgList&,
	                          Env&,
	                          int[3],
	                          int[2],
	                          int&);
	bool    handleGlexecEnvironment(pid_t, Env&, int[2], int);
	void    cleanupAfterGlexec(Claim *);
#endif

	void	initRunData( void );

	int	startKillTimer( int timeout );		// Timer for how long we're willing
	void	cancelKillTimer( void );	// to "hardkill" before we SIGKILL
	int startSoftkillTimeout( int timeout );
		// choose EXECUTE directory for starter
	void    finalizeExecuteDir( Claim * );

		// data that will be the same across all instances of this
		// starter (i.e. things that are valid for copying)
	ClassAd* s_ad; // starter capabilities ad, (not the job ad!)
	char*	s_path;
	bool 	s_is_dc;

		// data that only makes sense once this Starter object has
		// been assigned to a given resource and spawned.
	MyString        s_dpf; // prefix for all dprintf messages (normally the slot id)
	pid_t           s_pid;
	ProcFamilyUsage s_usage;
	time_t          s_birthdate;
	double          s_vm_cpu_usage;
	int             s_num_vm_cpus; // number of CPUs allocated to the hypervisor, used with additional_cpu_usage correction
	int             s_kill_tid;		// DC timer id for hard killing
	int             s_softkill_tid;
	int             s_hold_timeout;
	int             s_port1;
	int             s_port2;
	bool            s_is_vm_universe;
#if HAVE_BOINC
	bool            s_is_boinc;
#endif /* HAVE_BOINC */
	bool            s_was_reaped;
	int             s_reaper_id;
	int             s_exit_status;
	ClassAd *       s_orphaned_jobad;  // the job ad is transferred from the Claim to here if the claim is deleted before the starter is reaped.
	ReliSock*       s_job_update_sock;
	MyString        s_execute_dir;
	MyString        s_encrypted_execute_dir;
	DCMsgCallback*  m_hold_job_cb;
	std::string     m_starter_addr;
};

// living (or unreaped) starters live in a global data structure and can be looked up by PID.
Starter *findStarterByPid(pid_t pid);

#endif /* _CONDOR_STARTD_STARTER_H */
