/* hs_request.c - Add request and activate functionality to HostServ,
 *
 *
 * (C) 2003-2011 Anope Team
 * Contact us at team@anope.org
 *
 * Based on the original module by Rob <rob@anope.org>
 * Included in the Anope module pack since Anope 1.7.11
 * Anope Coder: GeniusDex <geniusdex@anope.org>
 *
 * Please read COPYING and README for further details.
 *
 * Send bug reports to the Anope Coder instead of the module
 * author, because any changes since the inclusion into anope
 * are not supported by the original author.
 */

#include "module.h"

static bool HSRequestMemoUser = false;
static bool HSRequestMemoOper = false;

void my_add_host_request(const Anope::string &nick, const Anope::string &vIdent, const Anope::string &vhost, const Anope::string &creator, time_t tmp_time);
void req_send_memos(CommandSource &source, const Anope::string &vIdent, const Anope::string &vHost);

struct HostRequest
{
	Anope::string ident;
	Anope::string host;
	time_t time;
};

typedef std::map<Anope::string, HostRequest *, std::less<ci::string> > RequestMap;
RequestMap Requests;

static Module *me;

class CommandHSRequest : public Command
{
	bool isvalidchar(char c)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')
			return true;
		return false;
	}

 public:
	CommandHSRequest() : Command("REQUEST", 1, 1)
	{
		this->SetDesc(_("Request a vHost for your nick"));
	}

	CommandReturn Execute(CommandSource &source, const std::vector<Anope::string> &params)
	{
		User *u = source.u;

		Anope::string rawhostmask = params[0];
		Anope::string hostmask;

		Anope::string vIdent = myStrGetToken(rawhostmask, '@', 0); /* Get the first substring, @ as delimiter */
		if (!vIdent.empty())
		{
			rawhostmask = myStrGetTokenRemainder(rawhostmask, '@', 1); /* get the remaining string */
			if (rawhostmask.empty())
			{
				me->SendMessage(source, _("Syntax: \002REQUEST \037vhost\037\002"));
				return MOD_CONT;
			}
			if (vIdent.length() > Config->UserLen)
			{
				source.Reply(_(HOST_SET_IDENTTOOLONG), Config->UserLen);
				return MOD_CONT;
			}
			else
				for (Anope::string::iterator s = vIdent.begin(), s_end = vIdent.end(); s != s_end; ++s)
					if (!isvalidchar(*s))
					{
						source.Reply(_(HOST_SET_IDENT_ERROR));
						return MOD_CONT;
					}
			if (!ircd->vident)
			{
				source.Reply(_(HOST_NO_VIDENT));
				return MOD_CONT;
			}
		}
		if (rawhostmask.length() < Config->HostLen)
			hostmask = rawhostmask;
		else
		{
			source.Reply(_(HOST_SET_TOOLONG), Config->HostLen);
			return MOD_CONT;
		}

		if (!isValidHost(hostmask, 3))
		{
			source.Reply(_(HOST_SET_ERROR));
			return MOD_CONT;
		}

		if (HSRequestMemoOper && Config->MSSendDelay > 0 && u && u->lastmemosend + Config->MSSendDelay > Anope::CurTime)
		{
			me->SendMessage(source, _("Please wait %d seconds before requesting a new vHost"), Config->MSSendDelay);
			u->lastmemosend = Anope::CurTime;
			return MOD_CONT;
		}
		my_add_host_request(u->nick, vIdent, hostmask, u->nick, Anope::CurTime);

		me->SendMessage(source, _("Your vHost has been requested"));
		req_send_memos(source, vIdent, hostmask);
		Log(LOG_COMMAND, u, this, NULL) << "to request new vhost " << (!vIdent.empty() ? vIdent + "@" : "") << hostmask;

		return MOD_CONT;
	}

	bool OnHelp(CommandSource &source, const Anope::string &subcommand)
	{
		me->SendMessage(source, _("Syntax: \002REQUEST \037vhost\037\002"));
		me->SendMessage(source, " ");
		me->SendMessage(source, _("Request the given vHost to be actived for your nick by the\n"
			"network administrators. Please be patient while your request\n"
			"is being considered."));
		return true;
	}

	void OnSyntaxError(CommandSource &source, const Anope::string &subcommand)
	{
		me->SendMessage(source, _("Syntax: \002REQUEST \037vhost\037\002"));
	}
};

class CommandHSActivate : public Command
{
 public:
	CommandHSActivate() : Command("ACTIVATE", 1, 1, "hostserv/set")
	{
		this->SetDesc(_("Approve the requested vHost of a user"));
	}

	CommandReturn Execute(CommandSource &source, const std::vector<Anope::string> &params)
	{
		User *u = source.u;

		const Anope::string &nick = params[0];

		NickAlias *na = findnick(nick);
		if (na)
		{
			RequestMap::iterator it = Requests.find(na->nick);
			if (it != Requests.end())
			{
				na->hostinfo.SetVhost(it->second->ident, it->second->host, u->nick, it->second->time);
				FOREACH_MOD(I_OnSetVhost, OnSetVhost(na));

				if (HSRequestMemoUser)
					memo_send(source, na->nick, _("[auto memo] Your requested vHost has been approved."), 2);

				me->SendMessage(source, _("vHost for %s has been activated"), na->nick.c_str());
				Log(LOG_COMMAND, u, this, NULL) << "for " << na->nick << " for vhost " << (!it->second->ident.empty() ? it->second->ident + "@" : "") << it->second->host;
				delete it->second;
				Requests.erase(it);
			}
			else
				me->SendMessage(source, _("No request for nick %s found."), nick.c_str());
		}
		else
			source.Reply(_(NICK_X_NOT_REGISTERED), nick.c_str());

		return MOD_CONT;
	}

	bool OnHelp(CommandSource &source, const Anope::string &subcommand)
	{
		me->SendMessage(source, _("Syntax: \002ACTIVATE \037nick\037\002"));
		me->SendMessage(source, " ");
		me->SendMessage(source, _("Activate the requested vHost for the given nick."));
		if (HSRequestMemoUser)
			me->SendMessage(source, _("A memo informing the user will also be sent."));

		return true;
	}

	void OnSyntaxError(CommandSource &source, const Anope::string &subcommand)
	{
		me->SendMessage(source, _("Syntax: \002ACTIVATE \037nick\037\002"));
	}
};

class CommandHSReject : public Command
{
 public:
	CommandHSReject() : Command("REJECT", 1, 2, "hostserv/set")
	{
		this->SetDesc(_("Reject the requested vHost of a user"));
	}

	CommandReturn Execute(CommandSource &source, const std::vector<Anope::string> &params)
	{
		User *u = source.u;

		const Anope::string &nick = params[0];
		const Anope::string &reason = params.size() > 1 ? params[1] : "";

		RequestMap::iterator it = Requests.find(nick);
		if (it != Requests.end())
		{
			delete it->second;
			Requests.erase(it);

			if (HSRequestMemoUser)
			{
				char message[BUFSIZE];
				if (!reason.empty())
					snprintf(message, sizeof(message), _("[auto memo] Your requested vHost has been rejected. Reason: %s"), reason.c_str());
				else
					snprintf(message, sizeof(message), "%s", _("[auto memo] Your requested vHost has been rejected."));

				memo_send(source, nick, message, 2);
			}

			me->SendMessage(source, _("vHost for %s has been rejected"), nick.c_str());
			Log(LOG_COMMAND, u, this, NULL) << "to reject vhost for " << nick << " (" << (!reason.empty() ? reason : "") << ")";
		}
		else
			me->SendMessage(source, _("No request for nick %s found."), nick.c_str());

		return MOD_CONT;
	}

	bool OnHelp(CommandSource &source, const Anope::string &subcommand)
	{
		me->SendMessage(source, _("Syntax: \002REJECT \037nick\037\002"));
		me->SendMessage(source, " ");
		me->SendMessage(source, _("Reject the requested vHost for the given nick."));
		if (HSRequestMemoUser)
			me->SendMessage(source, _("A memo informing the user will also be sent."));

		return true;
	}

	void OnSyntaxError(CommandSource &source, const Anope::string &subcommand)
	{
		me->SendMessage(source, _("Syntax: \002REJECT \037nick\037\002"));
	}
};

class HSListBase : public Command
{
 protected:
	CommandReturn DoList(CommandSource &source)
	{
		int counter = 1;
		int from = 0, to = 0;
		unsigned display_counter = 0;

		for (RequestMap::iterator it = Requests.begin(), it_end = Requests.end(); it != it_end; ++it)
		{
			HostRequest *hr = it->second;
			if (((counter >= from && counter <= to) || (!from && !to)) && display_counter < Config->NSListMax)
			{
				++display_counter;
				if (!hr->ident.empty())
					source.Reply(_("#%d Nick:\002%s\002, vhost:\002%s\002@\002%s\002 (%s - %s)"), counter, it->first.c_str(), hr->ident.c_str(), hr->host.c_str(), it->first.c_str(), do_strftime(hr->time).c_str());
				else
					source.Reply(_("#%d Nick:\002%s\002, vhost:\002%s\002 (%s - %s)"), counter, it->first.c_str(), hr->host.c_str(), it->first.c_str(), do_strftime(hr->time).c_str());
			}
			++counter;
		}
		source.Reply(_("Displayed all records (Count: \002%d\002)"), display_counter);

		return MOD_CONT;
	}
 public:
	HSListBase(const Anope::string &cmd, int min, int max) : Command(cmd, min, max, "hostserv/set")
	{
	}
};

class CommandHSWaiting : public HSListBase
{
 public:
	CommandHSWaiting() : HSListBase("WAITING", 0, 0)
	{
		this->SetDesc(_("Convenience command for LIST +req"));
	}

	CommandReturn Execute(CommandSource &source, const std::vector<Anope::string> &params)
	{
		return this->DoList(source);
	}

	bool OnHelp(CommandSource &source, const Anope::string &subcommand)
	{
		me->SendMessage(source, _("Syntax: \002WAITING\002"));
		me->SendMessage(source, " ");
		me->SendMessage(source, _("This command is provided for convenience. It is essentially\n"
			"the same as performing a LIST +req ."));

		return true;
	}
};

class HSRequest : public Module
{
	CommandHSRequest commandhsrequest;
	CommandHSActivate commandhsactive;
	CommandHSReject commandhsreject;
	CommandHSWaiting commandhswaiting;

 public:
	HSRequest(const Anope::string &modname, const Anope::string &creator) : Module(modname, creator)
	{
		me = this;

		this->AddCommand(HostServ, &commandhsrequest);
		this->AddCommand(HostServ, &commandhsactive);
		this->AddCommand(HostServ, &commandhsreject);
		this->AddCommand(HostServ, &commandhswaiting);

		this->SetAuthor("Anope");
		this->SetType(SUPPORTED);

		this->OnReload(false);

		Implementation i[] = { I_OnPreCommand, I_OnDatabaseRead, I_OnDatabaseWrite, I_OnReload };
		ModuleManager::Attach(i, this, 4);
	}

	~HSRequest()
	{
		/* Clean up all open host requests */
		while (!Requests.empty())
		{
			delete Requests.begin()->second;
			Requests.erase(Requests.begin());
		}
	}

	EventReturn OnPreCommand(CommandSource &source, Command *command, const std::vector<Anope::string> &params)
	{
		BotInfo *service = source.owner;
		if (service == HostServ)
		{
			if (command->name.equals_ci("LIST"))
			{
				Anope::string key = params.size() ? params[0] : "";

				if (!key.empty() && key.equals_ci("+req"))
				{
					std::vector<Anope::string> emptyParams;
					Command *c = FindCommand(HostServ, "WAITING");
					if (!c)
						throw CoreException("No waiting command?");
					c->Execute(source, emptyParams);
					return EVENT_STOP;
				}
			}
		}
		else if (service == NickServ)
		{
			if (command->name.equals_ci("DROP"))
			{
				NickAlias *na = findnick(source.u->nick);

				if (na)
				{
					RequestMap::iterator it = Requests.find(na->nick);

					if (it != Requests.end())
					{
						delete it->second;
						Requests.erase(it);
					}
				}
			}
		}

		return EVENT_CONTINUE;
	}

	EventReturn OnDatabaseRead(const std::vector<Anope::string> &params)
	{
		if (params[0].equals_ci("HS_REQUEST") && params.size() >= 5)
		{
			Anope::string vident = params[2].equals_ci("(null)") ? "" : params[2];
			my_add_host_request(params[1], vident, params[3], params[1], params[4].is_pos_number_only() ? convertTo<time_t>(params[4]) : 0);

			return EVENT_STOP;
		}

		return EVENT_CONTINUE;
	}

	void OnDatabaseWrite(void (*Write)(const Anope::string &))
	{
		for (RequestMap::iterator it = Requests.begin(), it_end = Requests.end(); it != it_end; ++it)
		{
			HostRequest *hr = it->second;
			std::stringstream buf;
			buf << "HS_REQUEST " << it->first << " " << (hr->ident.empty() ? "(null)" : hr->ident) << " " << hr->host << " " << hr->time;
			Write(buf.str());
		}
	}

	void OnReload(bool)
	{
		ConfigReader config;
		HSRequestMemoUser = config.ReadFlag("hs_request", "memouser", "no", 0);
		HSRequestMemoOper = config.ReadFlag("hs_request", "memooper", "no", 0);

		Log(LOG_DEBUG) << "[hs_request] Set config vars: MemoUser=" << HSRequestMemoUser << " MemoOper=" <<  HSRequestMemoOper;
	}
};

void req_send_memos(CommandSource &source, const Anope::string &vIdent, const Anope::string &vHost)
{
	Anope::string host;
	std::list<std::pair<Anope::string, Anope::string> >::iterator it, it_end;

	if (!vIdent.empty())
		host = vIdent + "@" + vHost;
	else
		host = vHost;

	if (HSRequestMemoOper == 1)
		for (unsigned i = 0; i < Config->Opers.size(); ++i)
		{
			Oper *o = Config->Opers[i];
			
			NickAlias *na = findnick(o->name);
			if (!na)
				continue;

			char message[BUFSIZE];
			snprintf(message, sizeof(message), _("[auto memo] vHost \002%s\002 has been requested."), host.c_str());
			memo_send(source, na->nick, message, 2);
		}
}

void my_add_host_request(const Anope::string &nick, const Anope::string &vIdent, const Anope::string &vhost, const Anope::string &creator, time_t tmp_time)
{
	HostRequest *hr = new HostRequest;
	hr->ident = vIdent;
	hr->host = vhost;
	hr->time = tmp_time;
	RequestMap::iterator it = Requests.find(nick);
	if (it != Requests.end())
	{
		delete it->second;
		Requests.erase(it);
	}
	Requests.insert(std::make_pair(nick, hr));
}

void my_load_config()
{
	ConfigReader config;
	HSRequestMemoUser = config.ReadFlag("hs_request", "memouser", "no", 0);
	HSRequestMemoOper = config.ReadFlag("hs_request", "memooper", "no", 0);

	Log(LOG_DEBUG) << "[hs_request] Set config vars: MemoUser=" << HSRequestMemoUser << " MemoOper=" <<  HSRequestMemoOper;
}

MODULE_INIT(HSRequest)
