// SPDX-License-Identifier: GPL-2.0
/*
 * planner-cli: headless Subsurface dive planner.
 *
 * Reads a text based dive plan (depth changes, gas mixtures, deco
 * settings) and runs the same core code the Subsurface GUI planner uses
 * (core/planner.cpp, core/deco.cpp, core/profile.cpp).  It outputs the
 * resulting dive profile: deco schedule, runtime, gas consumption,
 * per-sample ceilings, tissue saturations etc.
 *
 * Intended as a reference implementation against which a dive computer's
 * decompression algorithm can be validated.
 *
 * A plan file is a line based language; '#' or ';' start comments:
 *
 *   # settings
 *   deco-mode buehlmann          # or: vpm, recreational
 *   gflow 30
 *   gfhigh 70
 *   conservatism 3               # VPM-B only
 *   bottom-sac 20                # l/min
 *   deco-sac 17
 *   surface-pressure 1013        # mbar
 *   salinity 10300               # g/l (10 300 = sea water)
 *   descent-rate 20              # m/min
 *   ascent-rate 10               # m/min
 *   ascent-rate-6m 6             # last 6 m
 *   last-stop 6                  # 3 m or 6 m
 *   timestep 60                  # deco stop granularity, s
 *   mode ccr                     # oc (default), ccr, pscr
 *   setpoint 1.3                 # bar (rebreather)
 *   simulate off                 # on: treat plan as the dive as it was
 *                                #     dived and don't add deco
 *   # cylinders (index = order of definition)
 *   cyl air 15l 210bar 232bar    # mix, size, start pressure, working pressure
 *   cyl ean50 12l 200bar use=decostage
 *   cyl-change 1 21m             # deco gas becomes available at 21 m
 *
 *   # the dive: descend, then hold segments
 *   go 30m gas=0                 # descend at descent-rate, on cylinder 0
 *   hold 20min                   # bottom time at 30 m
 *   # (ascent and deco schedule are planned automatically)
 *
 * Output: human readable text summary (default), --csv per-sample
 * profile data, --json all of the above in a machine readable format.
 * --output FILE writes to a file instead of stdout.
 */

#include "core/dive.h"
#include "core/deco.h"
#include "core/equipment.h"
#include "core/errorhelper.h"
#include "core/event.h"
#include "core/gas.h"
#include "core/planner.h"
#include "core/profile.h"
#include "core/pref.h"
#include "core/subsurfacestartup.h"
#include "core/units.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include <git2.h>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

[[noreturn]] static void die(const std::string &msg)
{
	std::cerr << "planner-cli: " << msg << std::endl;
	exit(1);
}static std::string lower(std::string s)
{
	for (auto &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

static bool parse_number(std::string s, double &out)
{
	for (auto &c : s)
		if (c == ',')
			c = '.';
	size_t pos = 0;
	try {
		out = std::stod(s, &pos);
	} catch (...) {
		return false;
	}
	return pos == s.size();
}

// depth token: "30", "30m", "100ft", "100'" -> mm
static bool parse_depth(const std::string &tok, int &mm)
{
	bool ft = false;
	std::string s = tok;
	if (s.size() >= 2 && lower(s.substr(s.size() - 2)) == "ft") {
		s = s.substr(0, s.size() - 2);
		ft = true;
	} else if (!s.empty() && s.back() == 'm') {
		s.pop_back();
	} else if (!s.empty() && s.back() == '\'') {
		s.pop_back();
		ft = true;
	}
	double v;
	if (!parse_number(s, v))
		return false;
	mm = ft ? lrint(feet_to_mm(v)) : lrint(v * 1000.0);
	return true;
}

// duration token: "600", "10min", "600s", "1min30s" (bare number = seconds)
static bool parse_duration(const std::string &tok, int &seconds)
{
	int total = 0;
	size_t i = 0;
	while (i < tok.size()) {
		size_t j = i;
		while (j < tok.size() && (isdigit((unsigned char)tok[j]) || tok[j] == '.' || tok[j] == ','))
			j++;
		if (j == i)
			return false;
		double v;
		if (!parse_number(tok.substr(i, j - i), v))
			return false;
		size_t k = j;
		while (k < tok.size() && !isdigit((unsigned char)tok[k]) && tok[k] != '.' && tok[k] != ',')
			k++;
		std::string unit = tok.substr(j, k - j);
		if (unit.empty() || unit == "s" || unit == "sec")
			total += lrint(v);
		else if (unit == "min" || unit == "m")
			total += lrint(v * 60);
		else if (unit == "h")
			total += lrint(v * 3600);
		else
			return false;
		i = k;
	}
	seconds = total;
	return true;
}

// pressure token: "210", "210bar" -> mbar; "3000psi" -> mbar
static bool parse_pressure(const std::string &tok, int &mbar)
{
	bool psi = false;
	std::string s = tok;
	if (s.size() >= 3 && lower(s.substr(s.size() - 3)) == "psi") {
		s = s.substr(0, s.size() - 3);
		psi = true;
	} else if (s.size() >= 3 && lower(s.substr(s.size() - 3)) == "bar") {
		s = s.substr(0, s.size() - 3);
	} else if (!s.empty() && s.back() == 'r' && s.size() >= 1) {
		; // "bar" handled above
	}
	double v;
	if (!parse_number(s, v))
		return false;
	mbar = psi ? lrint(v * 68.9476) : lrint(v * 1000.0);
	return true;
}

// volume token: "15", "15l", "80cuft" -> ml
static bool parse_volume(const std::string &tok, int &mliter)
{
	bool cuft = false;
	std::string s = tok;
	if (s.size() >= 4 && lower(s.substr(s.size() - 4)) == "cuft") {
		s = s.substr(0, s.size() - 4);
		cuft = true;
	} else if (!s.empty() && s.back() == 'l') {
		s.pop_back();
	}
	double v;
	if (!parse_number(s, v))
		return false;
	mliter = cuft ? lrint(v * 28316.8466) : lrint(v * 1000.0);
	return true;
}

// surface pressure token: bare number or "850mb"/"1013mbar" are mbar,
// "0.85bar", "0.85atm" and "12.4psi" are converted
static bool parse_surface_pressure(const std::string &tok, double &mbar)
{
	std::string s = lower(tok);
	double factor = 1.0; // bare numbers are mbar
	if (s.size() >= 4 && s.substr(s.size() - 4) == "mbar")
		s = s.substr(0, s.size() - 4);
	else if (s.size() >= 2 && s.substr(s.size() - 2) == "mb")
		s = s.substr(0, s.size() - 2);
	else if (s.size() >= 3 && s.substr(s.size() - 3) == "bar") {
		s = s.substr(0, s.size() - 3);
		factor = 1000.0;
	} else if (s.size() >= 3 && s.substr(s.size() - 3) == "psi") {
		s = s.substr(0, s.size() - 3);
		factor = 68.9476;
	} else if (s.size() >= 3 && s.substr(s.size() - 3) == "atm") {
		s = s.substr(0, s.size() - 3);
		factor = 1013.25;
	}
	double v;
	if (!parse_number(s, v))
		return false;
	mbar = v * factor;
	// plausible range: high-pressure weather system down to ~Everest lakes
	return mbar >= 300.0 && mbar <= 1100.0;
}

// a parsed command line: command, positional arguments, key=value attributes
struct ParsedLine {
	std::string cmd;
	std::vector<std::string> pos;
	std::map<std::string, std::string> attrs;
};

static void parse_line(const std::string &line, int lineno, ParsedLine &out)
{
	out = ParsedLine();
	std::istringstream ss(line);
	std::string w;
	while (ss >> w) {
		size_t eq = w.find('=');
		if (eq != std::string::npos) {
			out.attrs[lower(w.substr(0, eq))] = w.substr(eq + 1);
		} else if (out.cmd.empty()) {
			out.cmd = lower(w);
		} else {
			out.pos.push_back(w);
		}
	}
}

static double attr_num(const ParsedLine &l, const std::string &key, double dflt)
{
	auto it = l.attrs.find(key);
	if (it == l.attrs.end())
		return dflt;
	double v;
	if (!parse_number(it->second, v))
		die("attribute " + key + " is not a number: " + it->second);
	return v;
}

static bool attr_bool(const ParsedLine &l, const std::string &key, bool dflt)
{
	auto it = l.attrs.find(key);
	if (it == l.attrs.end())
		return dflt;
	std::string v = lower(it->second);
	if (v == "true" || v == "on" || v == "yes" || v == "1")
		return true;
	if (v == "false" || v == "off" || v == "no" || v == "0")
		return false;
	return dflt;
}

// ---------------------------------------------------------------------------
// plan model
// ---------------------------------------------------------------------------

struct CylSpec {
	int o2 = 210, he = 0;       // permille
	int volume_ml = 15000;
	int start_mbar = 210000, wp_mbar = 232000;
	int change_depth_mm = -1;   // -1: no automatic gas change waypoint
	enum cylinderuse use = OC_GAS;
};

struct PlanInput {
	int gflow = 30, gfhigh = 70;
	int conservatism = 0;
	enum deco_mode deco_mode = BUEHLMANN;
	int bottom_sac_mlmin = 20000, deco_sac_mlmin = 17000;
	int surface_pressure_mbar = 1013;
	int salinity = 10300;
	int descrate_mms = 300;      // mm/sec
	int ascrate_mms = 150, ascratestops_mms = 150, ascrate6m_mms = 150;
	int last_stop_mm = 6000;     // 3m or 6m
	bool safetystop = false;
	bool o2breaks = false;
	bool dobailout = false;
	int timestep = 60;
	enum divemode_t divemode = OC;
	int setpoint_mbar = 1300;
	bool simulate = false;

	std::vector<CylSpec> cylinders;
	int bottom_cyl = 0;

	struct Wp {
		enum Kind { Go, Hold } kind;
		int depth_mm = 0;     // Go
		int duration = 0;     // Go: transit time (if explicit); Hold: hold time
		double rate_mpmin = 0; // optional explicit rate (m/min), 0 = default
		bool time_explicit = false;
		int cylinder = -1;
		int po2_mbar = -1;
		enum divemode_t divemode = UNDEF_COMP_TYPE;
	};
	std::vector<Wp> waypoints;
};

// gas names: air, o2, oxygen, ean32, nx32, e32, tx15/45, tmx18/45, 15/45
static bool parse_gasmix(std::string mix, int &o2, int &he)
{
	mix = lower(mix);
	if (mix == "air") {
		o2 = 210;
		he = 0;
		return true;
	}
	if (mix == "o2" || mix == "oxygen") {
		o2 = 1000;
		he = 0;
		return true;
	}
	bool is_tx = mix.find('/') != std::string::npos;
	if (mix.rfind("tmx", 0) == 0)
		mix = mix.substr(3);
	else if (mix.rfind("tx", 0) == 0)
		mix = mix.substr(2);
	else if (mix.rfind("ean", 0) == 0)
		mix = mix.substr(3);
	else if (mix.rfind("nx", 0) == 0)
		mix = mix.substr(2);
	else if (mix.rfind("e", 0) == 0)
		mix = mix.substr(1);

	double o2p, hep;
	if (is_tx) {
		size_t slash = mix.find('/');
		if (!parse_number(mix.substr(0, slash), o2p) ||
		    !parse_number(mix.substr(slash + 1), hep))
			return false;
		he = lrint(hep * 10);
	} else {
		if (!parse_number(mix, o2p))
			return false;
		he = 0;
	}
	o2 = lrint(o2p * 10);
	return o2 >= 50 && o2 <= 1000 && he >= 0 && o2 + he <= 1000;
}

static enum divemode_t parse_divemode(const std::string &s)
{
	std::string m = lower(s);
	if (m == "oc" || m == "open")
		return OC;
	if (m == "ccr" || m == "closed" || m == "rebreather")
		return CCR;
	if (m == "pscr")
		return PSCR;
	die("unknown dive mode: " + s + " (use oc, ccr or pscr)");
}

static enum cylinderuse parse_cyl_use(const std::string &s)
{
	std::string m = lower(s);
	if (m.empty() || m == "oc" || m == "bottom")
		return OC_GAS;
	if (m == "diluent" || m == "dil")
		return DILUENT;
	if (m == "oxygen" || m == "o2")
		return OXYGEN;
	if (m == "notused" || m == "not-used" || m == "unused")
		return NOT_USED;
	// the planner only knows these use types; "stage"/"deco"/"bailout"
	// are common aliases divers use - treat them as OC deco gases
	if (m == "stage" || m == "deco" || m == "bailout" || m == "decostage")
		return OC_GAS;
	die("unknown cylinder use: " + s);
}

// ---------------------------------------------------------------------------
// plan file parser
// ---------------------------------------------------------------------------

static PlanInput parse_plan(std::istream &in)
{
	PlanInput plan;
	std::string raw;
	int lineno = 0;

	while (std::getline(in, raw)) {
		lineno++;
		size_t cpos = raw.find_first_of("#;");
		// ignore shell hebang
		if (lineno == 1 && raw.rfind("#!", 0) == 0)
			continue;
		std::string line = cpos == std::string::npos ? raw : raw.substr(0, cpos);
		ParsedLine l;
		parse_line(line, lineno, l);
		if (l.cmd.empty())
			continue;
		std::string where = "line " + std::to_string(lineno) + ": ";

		if (l.cmd == "deco-mode" || l.cmd == "deco") {
			if (l.pos.empty())
				die(where + "deco-mode needs buehlmann, vpm or recreational");
			std::string m = lower(l.pos[0]);
			if (m == "buehlmann" || m == "buhlmann" || m == "zhl")
				plan.deco_mode = BUEHLMANN;
			else if (m == "vpm" || m == "vpmb")
				plan.deco_mode = VPMB;
			else if (m == "recreational")
				plan.deco_mode = RECREATIONAL;
			else
				die(where + "unknown deco mode " + l.pos[0]);
		} else if (l.cmd == "gflow" || l.cmd == "gf-low") {
			double v = l.pos.empty() ? attr_num(l, "value", plan.gflow) :
				(l.attrs.empty() ? atof(l.pos[0].c_str()) : attr_num(l, "value", plan.gflow));
			plan.gflow = lrint(v);
		} else if (l.cmd == "gfhigh" || l.cmd == "gf-high") {
			double v = l.pos.empty() ? attr_num(l, "value", plan.gfhigh) :
				(l.attrs.empty() ? atof(l.pos[0].c_str()) : attr_num(l, "value", plan.gfhigh));
			plan.gfhigh = lrint(v);
		} else if (l.cmd == "conservatism") {
			double v = l.pos.empty() ? attr_num(l, "value", plan.conservatism) : atof(l.pos[0].c_str());
			plan.conservatism = lrint(v);
		} else if (l.cmd == "bottom-sac" || l.cmd == "bottomsac") {
			double v = l.pos.empty() ? attr_num(l, "lpm", plan.bottom_sac_mlmin / 1000.0) : atof(l.pos[0].c_str());
			plan.bottom_sac_mlmin = lrint(v * 1000);
		} else if (l.cmd == "deco-sac" || l.cmd == "decosac") {
			double v = l.pos.empty() ? attr_num(l, "lpm", plan.deco_sac_mlmin / 1000.0) : atof(l.pos[0].c_str());
			plan.deco_sac_mlmin = lrint(v * 1000);
		} else if (l.cmd == "surface-pressure") {
			// positional form: bare numbers are mbar (1013 = sea level);
			// 0.85bar / 850mb / 12.4psi suffixes are also accepted
			if (l.pos.empty()) {
				plan.surface_pressure_mbar = lrint(attr_num(l, "mbar", plan.surface_pressure_mbar));
			} else {
				double mbar;
				if (!parse_surface_pressure(l.pos[0], mbar))
					die(where + "cannot parse surface pressure " + l.pos[0]);
				plan.surface_pressure_mbar = lrint(mbar);
			}
		} else if (l.cmd == "salinity") {
			double v = l.pos.empty() ? attr_num(l, "value", plan.salinity) : atof(l.pos[0].c_str());
			plan.salinity = lrint(v);
		} else if (l.cmd == "descent-rate") {
			double v = l.pos.empty() ? attr_num(l, "mpm", plan.descrate_mms * 60 / 1000.0) : atof(l.pos[0].c_str());
			plan.descrate_mms = lrint(v * 1000 / 60);
		} else if (l.cmd == "ascent-rate") {
			double v = l.pos.empty() ? attr_num(l, "mpm", plan.ascrate_mms * 60 / 1000.0) : atof(l.pos[0].c_str());
			plan.ascrate_mms = lrint(v * 1000 / 60);
		} else if (l.cmd == "ascent-rate-stops") {
			double v = l.pos.empty() ? attr_num(l, "mpm", plan.ascratestops_mms * 60 / 1000.0) : atof(l.pos[0].c_str());
			plan.ascratestops_mms = lrint(v * 1000 / 60);
		} else if (l.cmd == "ascent-rate-6m") {
			double v = l.pos.empty() ? attr_num(l, "mpm", plan.ascrate6m_mms * 60 / 1000.0) : atof(l.pos[0].c_str());
			plan.ascrate6m_mms = lrint(v * 1000 / 60);
		} else if (l.cmd == "last-stop") {
			if (l.pos.empty())
				die(where + "last-stop needs 3 or 6 (m)");
			int mm;
			if (!parse_depth(l.pos[0], mm))
				die(where + "cannot parse last stop depth " + l.pos[0]);
			plan.last_stop_mm = mm;
		} else if (l.cmd == "safety-stop") {
			plan.safetystop = l.pos.empty() ? attr_bool(l, "value", true) :
				(lower(l.pos[0]) == "on" || lower(l.pos[0]) == "true");
		} else if (l.cmd == "o2-breaks") {
			plan.o2breaks = l.pos.empty() ? attr_bool(l, "value", true) :
				(lower(l.pos[0]) == "on" || lower(l.pos[0]) == "true");
		} else if (l.cmd == "bailout" || l.cmd == "do-bailout") {
			plan.dobailout = l.pos.empty() ? attr_bool(l, "value", true) :
				(lower(l.pos[0]) == "on" || lower(l.pos[0]) == "true");
		} else if (l.cmd == "timestep") {
			double v = l.pos.empty() ? attr_num(l, "value", plan.timestep) : atof(l.pos[0].c_str());
			plan.timestep = std::clamp((int)lrint(v), 1, 3600);
		} else if (l.cmd == "mode") {
			if (l.pos.empty())
				die(where + "mode needs oc, ccr or pscr");
			plan.divemode = parse_divemode(l.pos[0]);
		} else if (l.cmd == "setpoint") {
			if (l.pos.empty())
				die(where + "setpoint needs a po2 in bar");
			double v = atof(l.pos[0].c_str());
			plan.setpoint_mbar = lrint(v * 1000);
		} else if (l.cmd == "simulate" || l.cmd == "as-dove") {
			if (l.pos.empty())
				plan.simulate = true;
			else
				plan.simulate = (lower(l.pos[0]) == "on" || lower(l.pos[0]) == "true");
		} else if (l.cmd == "cyl" || l.cmd == "cylinder" || l.cmd == "tank") {
			if (l.pos.empty())
				die(where + "cyl needs a gas mix, e.g. 'cyl air 15l 210bar'");
			CylSpec c;
			if (!parse_gasmix(l.pos[0], c.o2, c.he))
				die(where + "cannot parse gas mix \"" + l.pos[0] + "\"");
			if (l.pos.size() > 1 && !parse_volume(l.pos[1], c.volume_ml))
				die(where + "cannot parse size " + l.pos[1]);
			if (l.pos.size() > 2 && !parse_pressure(l.pos[2], c.start_mbar))
				die(where + "cannot parse start pressure " + l.pos[2]);
			if (l.pos.size() > 3 && !parse_pressure(l.pos[3], c.wp_mbar))
				die(where + "cannot parse working pressure " + l.pos[3]);
			if (l.attrs.count("size") && !parse_volume(l.attrs["size"], c.volume_ml))
				die(where + "cannot parse size=" + l.attrs["size"]);
			if (l.attrs.count("start") && !parse_pressure(l.attrs["start"], c.start_mbar))
				die(where + "cannot parse start=" + l.attrs["start"]);
			if (l.attrs.count("wp") && !parse_pressure(l.attrs["wp"], c.wp_mbar))
				die(where + "cannot parse wp=" + l.attrs["wp"]);
			c.use = parse_cyl_use(l.attrs.count("use") ? l.attrs["use"] : "");
			if (l.attrs.count("change") && !parse_depth(l.attrs["change"], c.change_depth_mm))
				die(where + "cannot parse change=" + l.attrs["change"]);
			plan.cylinders.push_back(c);
		} else if (l.cmd == "cyl-change" || l.cmd == "change") {
			if (l.pos.size() < 2)
				die(where + "cyl-change needs <cylinder-index> <depth>");
			int idx = atoi(l.pos[0].c_str());
			if (idx < 0 || idx >= (int)plan.cylinders.size())
				die(where + "cylinder index out of range");
			if (!parse_depth(l.pos[1], plan.cylinders[idx].change_depth_mm))
				die(where + "cannot parse depth " + l.pos[1]);
		} else if (l.cmd == "go" || l.cmd == "descend" || l.cmd == "ascend" || l.cmd == "to") {
			if (l.pos.empty())
				die(where + "go needs a depth, e.g. 'go 30m'");
			PlanInput::Wp w;
			w.kind = PlanInput::Wp::Go;
			if (!parse_depth(l.pos[0], w.depth_mm))
				die(where + "cannot parse depth " + l.pos[0]);
			if (l.attrs.count("gas"))
				w.cylinder = atoi(l.attrs["gas"].c_str());
			if (l.attrs.count("po2"))
				w.po2_mbar = lrint(atof(l.attrs["po2"].c_str()) * 1000);
			if (l.attrs.count("mode"))
				w.divemode = parse_divemode(l.attrs["mode"]);
			if (l.attrs.count("rate")) {
				w.rate_mpmin = atof(l.attrs["rate"].c_str());
				if (w.rate_mpmin <= 0)
					die(where + "rate must be positive (m/min)");
			}
			if (l.attrs.count("time")) {
				if (!parse_duration(l.attrs["time"], w.duration))
					die(where + "cannot parse time=" + l.attrs["time"]);
				w.time_explicit = true;
			}
			plan.waypoints.push_back(w);
		} else if (l.cmd == "hold" || l.cmd == "stay" || l.cmd == "wait") {
			if (l.pos.empty())
				die(where + "hold needs a duration, e.g. 'hold 20min'");
			PlanInput::Wp w;
			w.kind = PlanInput::Wp::Hold;
			if (!parse_duration(l.pos[0], w.duration))
				die(where + "cannot parse duration " + l.pos[0]);
			if (l.attrs.count("gas"))
				w.cylinder = atoi(l.attrs["gas"].c_str());
			if (l.attrs.count("po2"))
				w.po2_mbar = lrint(atof(l.attrs["po2"].c_str()) * 1000);
			if (l.attrs.count("mode"))
				w.divemode = parse_divemode(l.attrs["mode"]);
			plan.waypoints.push_back(w);
		} else {
			die(where + "unknown command \"" + l.cmd + "\"");
		}
	}
	return plan;
}

static void validate_plan(const PlanInput &plan)
{
	if (plan.cylinders.empty())
		die("plan defines no cylinders - add at least one 'cyl' line");
	if (plan.waypoints.empty())
		die("plan has no dive segments - add 'go' and 'hold' lines");
	if (plan.waypoints[0].kind != PlanInput::Wp::Go)
		die("plan must start with a 'go <depth>' segment (descend first)");

	// a surface waypoint is only meaningful when simulating; in planner
	// mode the ascent (including all deco stops) is being planned
	for (const auto &w : plan.waypoints)
		if (w.kind == PlanInput::Wp::Go && (w.depth_mm < 0 || (w.depth_mm == 0 && !plan.simulate)))
			die("waypoint depth must be positive (a waypoint at the surface requires 'simulate on')");

	bool has_bottom = false;
	for (const auto &w : plan.waypoints)
		if (w.kind == PlanInput::Wp::Go && w.depth_mm > 0)
			has_bottom = true;
	if (!has_bottom)
		die("plan never leaves the surface - start with e.g. 'go 30m'");

	for (const auto &w : plan.waypoints)
		if (w.cylinder >= (int)plan.cylinders.size())
			die("waypoint references cylinder " + std::to_string(w.cylinder) +
			    " but only " + std::to_string(plan.cylinders.size()) + " cylinders are defined");
}

// ---------------------------------------------------------------------------
// plan execution - mirrors what the Subsurface GUI planner does
// ---------------------------------------------------------------------------

struct Results {
	std::unique_ptr<struct dive> dive;
	std::unique_ptr<struct diveplan> dp;
	std::unique_ptr<struct deco_state> ds;
	plot_info pi;
	std::vector<decostop> stops;
	planner_error_t error = PLAN_OK;
};

static Results run_plan(const PlanInput &in)
{
	prefs = default_prefs;
	prefs.unit_system = METRIC;
	prefs.units.length = units::METERS;
	prefs.planner_deco_mode = in.deco_mode;
	prefs.gflow = in.gflow;
	prefs.gfhigh = in.gfhigh;
	prefs.vpmb_conservatism = in.conservatism;
	prefs.bottomsac = in.bottom_sac_mlmin;
	prefs.decosac = in.deco_sac_mlmin;
	prefs.descrate = in.descrate_mms;
	prefs.ascrate75 = in.ascrate_mms;
	prefs.ascrate50 = in.ascrate_mms;
	prefs.ascratestops = in.ascratestops_mms;
	prefs.ascratelast6m = in.ascrate6m_mms;
	prefs.last_stop = in.last_stop_mm > lrint(feet_to_mm(15)); // 6m
	prefs.safetystop = in.safetystop;
	prefs.doo2breaks = in.o2breaks;
	prefs.dobailout = in.dobailout;
	prefs.o2narcotic = false;
	// per-sample deco data, as displayed on the profile
	prefs.calcalltissues = true;
	prefs.calcceiling = true;
	prefs.calcceiling3m = false;
	prefs.calcndltts = true;
	prefs.pp_graphs.po2 = true;
	prefs.pp_graphs.pn2 = true;
	prefs.pp_graphs.phe = true;

	auto dive = std::make_unique<struct dive>();
	auto dp = std::make_unique<struct diveplan>();
	auto ds = std::make_unique<struct deco_state>();
	deco_state_cache cache;
	std::vector<decostop> stops;

	dive->dcs[0].divemode = in.divemode;
	dive->salinity = in.salinity;
	dive->surface_pressure.mbar = in.surface_pressure_mbar;

	dp->surface_pressure.mbar = in.surface_pressure_mbar;
	dp->salinity = in.salinity;
	dp->gflow = in.gflow;
	dp->gfhigh = in.gfhigh;
	dp->vpmb_conservatism = in.conservatism;
	dp->bottomsac = in.bottom_sac_mlmin;
	dp->decosac = in.deco_sac_mlmin;

	// declare cylinders before taking any cylinder pointers.
	// Note: in the planner, the tank is considered filled to its
	// working pressure (this mirrors the GUI planner which stores the
	// user chosen start pressure in type.workingpressure).
	for (int i = 0; i < (int)in.cylinders.size(); i++) {
		const CylSpec &cs = in.cylinders[i];
		cylinder_t *cyl = dive->get_or_create_cylinder(i);
		cyl->gasmix.o2.permille = cs.o2;
		cyl->gasmix.he.permille = cs.he;
		cyl->type.size.mliter = cs.volume_ml;
		cyl->type.workingpressure.mbar = cs.start_mbar;
		cyl->start.mbar = cs.start_mbar;
		cyl->cylinder_use = cs.use;
		cyl->manually_added = true;
	}
	reset_cylinders(dive.get(), true);

	// gas availability waypoints: the planner is told at which depth each
	// decocyl becomes usable (same as the gas_mod() calls in
	// tests/testplan.cpp setupPlan())
	for (int i = 0; i < (int)in.cylinders.size(); i++) {
		const CylSpec &cs = in.cylinders[i];
		if (i != in.bottom_cyl && cs.change_depth_mm >= 0)
			plan_add_segment(*dp, 0, depth_t { .mm = cs.change_depth_mm }, i,
					 in.divemode == CCR ? in.setpoint_mbar : 0, true, in.divemode);
	}

	// user waypoints -> diveplan segments
	int depth_mm = 0, cylinder = in.bottom_cyl, po2_mbar = in.setpoint_mbar;
	enum divemode_t divemode = in.divemode;
	if (in.divemode != CCR)
		po2_mbar = 0;
	for (const auto &w : in.waypoints) {
		int cyl_next = w.cylinder >= 0 ? w.cylinder : cylinder;
		enum divemode_t mode_next = w.divemode != UNDEF_COMP_TYPE ? w.divemode : divemode;
		int po2_next = w.po2_mbar >= 0 ? w.po2_mbar : (mode_next == CCR ? po2_mbar : 0);
		int po2_dp = mode_next == CCR ? po2_next : 0;

		if (w.kind == PlanInput::Wp::Go) {
			int transit;
			if (w.time_explicit) {
				transit = w.duration;
			} else {
				double rate_mm_s = w.rate_mpmin > 0 ? w.rate_mpmin * 1000.0 / 60.0 :
					(w.depth_mm > depth_mm ? in.descrate_mms : in.ascrate_mms);
				transit = lrint(std::abs((double)(w.depth_mm - depth_mm)) / std::max(rate_mm_s, 1.0));
			}
			plan_add_segment(*dp, transit, depth_t { .mm = w.depth_mm }, cyl_next, po2_dp, true, mode_next);
			depth_mm = w.depth_mm;
			cylinder = cyl_next;
			divemode = mode_next;
			po2_mbar = po2_next;
		} else { // Hold
			if (cyl_next != cylinder || mode_next != divemode) {
				// gas/setpoint/mode switch instantaneously at the
				// current position, then hold
				plan_add_segment(*dp, 0, depth_t { .mm = depth_mm }, cyl_next, po2_dp, true, mode_next);
				cylinder = cyl_next;
				divemode = mode_next;
				po2_mbar = po2_next;
			}
			plan_add_segment(*dp, w.duration, depth_t { .mm = depth_mm }, cylinder,
					 divemode == CCR ? po2_mbar : 0, true, divemode);
		}
	}

	// validate cylinder waypoints reference valid gases
	for (const auto &seg : dp->dp)
		if (seg.cylinderid >= (int)in.cylinders.size())
			die("waypoint references cylinder out of range");

	bool is_planner = !in.simulate;
	planner_error_t error = plan(ds.get(), *dp, dive.get(), 0, in.timestep, cache, is_planner, false, &stops);

	// fill in derived dive data (duration, mean depth, CNS, OTU)
	dive->fixup_dive();

	if (!is_planner) {
		// in simulation mode the plan stops after building the profile;
		// compute the tissue state the same way the planner does
		deco_state_cache fresh_cache;
		tissue_at_end(ds.get(), dive.get(), &dive->dcs[0], fresh_cache);
	}

	// per-sample deco info (ceiling, tissue saturation).  Passing the
	// final planner deco state mirrors the Subsurface profile widget.
	plot_info pi = create_plot_info_new(dive.get(), &dive->dcs[0], is_planner ? ds.get() : nullptr);

	Results res;
	res.dive = std::move(dive);
	res.dp = std::move(dp);
	res.ds = std::move(ds);
	res.pi = std::move(pi);
	res.stops = std::move(stops);
	res.error = error;
	return res;
}

// ---------------------------------------------------------------------------
// output helpers
// ---------------------------------------------------------------------------

static std::string fmt_time(int seconds)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
	if (seconds >= 3600)
		snprintf(buf, sizeof(buf), "%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
	return buf;
}

// ---------------------------------------------------------------------------
// text output
// ---------------------------------------------------------------------------

static void print_text(const PlanInput &in, const Results &res, std::ostream &os)
{
	const auto &dive = *res.dive;
	const auto &dc = dive.dcs[0];

	os << "Subsurface planner-cli reference output\n";
	os << "======================================\n\n";
	os << "Model:       " << (in.deco_mode == VPMB ? "VPM-B" :
				 in.deco_mode == RECREATIONAL ? "Recreational" : "Buehlmann");
	if (in.deco_mode != VPMB)
		os << ", gradient factors " << in.gflow << "/" << in.gfhigh;
	else
		os << ", conservatism +" << in.conservatism;
	os << "\n";
	os << "Mode:        " << (in.simulate ? "simulate (profile as dived)" : "planner (deco computed)") << "\n";
	os << "Dive type:   " << divemode_text[dc.divemode] << "\n";
	os << "Surface:     " << in.surface_pressure_mbar << " mbar, salinity "
	   << in.salinity / 1000.0 << " kg/l\n";
	os << "Rates:       descent " << in.descrate_mms / 1000.0 * 60 << " m/min, ascent "
	   << in.ascrate_mms / 1000.0 * 60 << " m/min, last 6 m "
	   << in.ascrate6m_mms / 1000.0 * 60 << " m/min\n";
	os << "SACs:        bottom " << in.bottom_sac_mlmin / 1000.0
	   << " l/min, deco " << in.deco_sac_mlmin / 1000.0 << " l/min\n";
	os << "Last stop:   " << in.last_stop_mm / 1000.0 << " m\n\n";

	os << "Runtime:     " << fmt_time(dc.duration.seconds)
	   << " (" << dc.duration.seconds << " s)\n";
	os << "Max depth:   " << dc.maxdepth.mm / 1000.0 << " m\n";
	os << "Avg depth:   " << dc.meandepth.mm / 1000.0 << " m\n";
	if (!in.simulate) {
		os << "First ceiling at end of bottom phase: "
		   << dive.mbar_to_depth(res.ds->first_ceiling_pressure.mbar).mm / 1000.0 << " m\n";
	}
	os << "\n";

	// deco schedule
	if (in.simulate) {
		int max_ceiling = 0;
		for (const auto &e : res.pi.entry)
			max_ceiling = std::max(max_ceiling, e.ceiling.mm);
		os << "Deepest ceiling during the dive: "
		   << max_ceiling / 1000.0 << " m "
		   << "(ceiling violations: "
		   << std::count_if(res.pi.entry.begin(), res.pi.entry.end(),
				    [](const plot_data &e) { return e.ceiling.mm - 100 > e.depth.mm && e.depth.mm > 1000; })
		   << " samples)\n\n";
	} else {
		os << "Deco schedule (planned):\n";
		int stop_count = 0;
		for (const auto &s : res.stops) {
			if (s.time <= 0)
				continue; // transition depths passed on the way up
			char minbuf[32];
			snprintf(minbuf, sizeof(minbuf), "%.1f", s.time / 60.0);
			os << "  " << s.depth / 1000.0 << " m: " << minbuf << " min\n";
			stop_count++;
		}
		if (!stop_count)
			os << "  (no deco stops required)\n";
		os << "\n";
	}

	// gas switches
	os << "Dive events:\n";
	for (const auto &ev : dc.events) {
		if (ev.type == SAMPLE_EVENT_GASCHANGE2 || ev.type == SAMPLE_EVENT_GASCHANGE) {
			std::string gas = ev.gas.index < (int)dive.cylinders.size() ?
				dive.get_cylinder(ev.gas.index)->gasmix.name() : "?";
			os << "  " << fmt_time(ev.time.seconds) << ": switch to " << gas
			   << " (cylinder " << ev.gas.index << ")\n";
		}
	}
	os << "\n";

	// gas usage
	os << "Gas consumption";
	if (in.simulate)
		os << " (not tracked in simulate mode; use planner mode to get gas data)";
	os << ":\n";
	for (size_t i = 0; i < dive.cylinders.size(); i++) {
		const cylinder_t &c = *dive.get_cylinder(i);
		if (!c.gas_used.mliter && !c.deco_gas_used.mliter && c.start.mbar == 0)
			continue;
		os << "  [" << i << "] " << c.gasmix.name()
		   << " (" << c.type.size.mliter / 1000.0 << " l): "
		   << c.start.mbar / 1000.0 << " bar -> " << c.end.mbar / 1000.0 << " bar"
		   << " (used " << c.gas_used.mliter / 1000.0 << " l";
		if (c.deco_gas_used.mliter > 0)
			os << ", of which deco " << c.deco_gas_used.mliter / 1000.0 << " l";
		os << ")\n";
	}
	double mingas_bar = 0;
	for (const auto &dp : res.dp->dp)
		if (dp.minimum_gas.mbar > static_cast<int32_t>(mingas_bar * 1000))
			mingas_bar = dp.minimum_gas.mbar / 1000.0;
	if (mingas_bar > 0)
		os << "  minimum gas / rock bottom (bottom cylinder): " << mingas_bar << " bar\n";
	os << "\n";

	// CNS / OTU: computed by fixup_dive() over the full profile
	os << "Max CNS: " << dive.cns << " %   OTU: " << dive.otu << "\n\n";

	// final tissue state
	os << "Tissue loadings when surfacing (bar):\n";
	os << "  N2 half-times:  5  8  12.5 18.5 27 38.5 54.5 77 109 146 187 239 305 387 498 635 min\n";
	os << "  He half-times:  1.9 3.2 5.1  7.6 11.1 15.9 22.5 31.9 45.4 60.6 77.5 98.8 126 160 205 261 min\n";
	os << "  Tissue        N2 sat      He sat   inert tot\n";
	for (int i = 0; i < 16; i++) {
		char buf[128];
		double n2 = res.ds->tissue_n2_sat[i];
		double he = res.ds->tissue_he_sat[i];
		double tot = res.ds->tissue_inertgas_saturation[i];
		snprintf(buf, sizeof(buf), "  %6d    %8.3f  %8.3f  %8.3f\n", i + 1, n2, he, tot);
		os << buf;
	}
}

// ---------------------------------------------------------------------------
// CSV output
// ---------------------------------------------------------------------------

static void print_csv_header(const Results &res, std::ostream &os)
{
	const auto &dive = *res.dive;
	os << "time_s,depth_m,ceiling_m,in_deco,ndl_s,tts_s,gf_percent,surface_gf_percent,cns_percent,"
	   << "po2_bar,pn2_bar,phe_bar,ambient_pressure_bar,density_g_per_l,sac_l_per_min";
	for (size_t i = 0; i < dive.cylinders.size(); i++)
		os << ",cyl" << i << "_pressure_bar";
	for (int t = 1; t <= 16; t++)
		os << ",sat_percent_t" << (t < 10 ? "0" : "") << t;
	for (int t = 1; t <= 16; t++)
		os << ",ceiling_m_t" << (t < 10 ? "0" : "") << t;
	os << "\n";
}

static void print_csv(const Results &res, std::ostream &os)
{
	print_csv_header(res, os);
	const auto &pi = res.pi;
	for (size_t idx = 0; idx < pi.entry.size(); idx++) {
		const auto &e = pi.entry[idx];
		if (e.sec < 0)
			continue;
		char buf[128];
		os << e.sec << ",";
		snprintf(buf, sizeof(buf), "%.2f,", e.depth.mm / 1000.0);
		os << buf;
		snprintf(buf, sizeof(buf), "%.2f,", e.ceiling.mm / 1000.0);
		os << buf;
		os << (e.in_deco ? "yes," : "no,");
		os << e.ndl << "," << e.tts << ",";
		snprintf(buf, sizeof(buf), "%.2f,%.2f,%d,", e.current_gf, e.surface_gf, e.cns);
		os << buf;
		snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f,%.2f,%.1f,",
			 e.pressures.o2, e.pressures.n2, e.pressures.he,
			 e.ambpressure, e.density, e.sac / 1000.0);
		os << buf;
		for (size_t c = 0; c < res.dive->cylinders.size(); c++) {
			snprintf(buf, sizeof(buf), "%.1f,", get_plot_sensor_pressure(pi, idx, c) / 1000.0);
			os << buf;
		}
		for (int t = 0; t < 16; t++) {
			snprintf(buf, sizeof(buf), "%d,", e.percentages[t]);
			os << buf;
		}
		for (int t = 0; t < 16; t++) {
			snprintf(buf, sizeof(buf), "%.2f%s", e.ceilings[t].mm / 1000.0, t == 15 ? "\n" : ",");
			os << buf;
		}
	}
}

// ---------------------------------------------------------------------------
// JSON output
// ---------------------------------------------------------------------------

static void json_escape(std::ostream &os, const std::string &s)
{
	os << '"';
	for (char c : s) {
		switch (c) {
		case '"': os << "\\\""; break;
		case '\\': os << "\\\\"; break;
		case '\n': os << "\\n"; break;
		case '\r': os << "\\r"; break;
		case '\t': os << "\\t"; break;
		default: os << c;
		}
	}
	os << '"';
}

static void print_json(const PlanInput &in, const Results &res, std::ostream &os)
{
	const auto &dive = *res.dive;
	const auto &dc = dive.dcs[0];
	const auto &pi = res.pi;

	os << "{\n  \"planner\": {\n";
	os << "    \"implementation\": \"Subsurface planner-cli\",\n";
	os << "    \"deco_mode\": \"" << (in.deco_mode == VPMB ? "VPM-B" :
		in.deco_mode == RECREATIONAL ? "recreational" : "buehlmann") << "\",\n";
	os << "    \"gflow\": " << in.gflow << ",\n";
	os << "    \"gfhigh\": " << in.gfhigh << ",\n";
	os << "    \"vpmb_conservatism\": " << in.conservatism << ",\n";
	os << "    \"divemode\": \"" << divemode_text[dc.divemode] << "\",\n";
	os << "    \"bottom_sac_ml_per_min\": " << in.bottom_sac_mlmin << ",\n";
	os << "    \"deco_sac_ml_per_min\": " << in.deco_sac_mlmin << ",\n";
	os << "    \"surface_pressure_mbar\": " << in.surface_pressure_mbar << ",\n";
	os << "    \"salinity_permille_kg_per_l\": " << in.salinity / 1000.0 << ",\n";
	os << "    \"descent_rate_mm_per_s\": " << in.descrate_mms << ",\n";
	os << "    \"ascent_rate_mm_per_s\": " << in.ascrate_mms << ",\n";
	os << "    \"ascent_rate_last6m_mm_per_s\": " << in.ascrate6m_mms << ",\n";
	os << "    \"last_stop_mm\": " << in.last_stop_mm << ",\n";
	os << "    \"timestep_s\": " << in.timestep << ",\n";
	os << "    \"simulate\": " << (in.simulate ? "true" : "false") << "\n";
	os << "  },\n";

	os << "  \"dive\": {\n";
	os << "    \"runtime_s\": " << dc.duration.seconds << ",\n";
	os << "    \"max_depth_m\": " << dc.maxdepth.mm / 1000.0 << ",\n";
	os << "    \"avg_depth_m\": " << dc.meandepth.mm / 1000.0 << ",\n";
	os << "    \"first_ceiling_m\": "
	   << dive.mbar_to_depth(res.ds->first_ceiling_pressure.mbar).mm / 1000.0 << ",\n";
	os << "    \"max_cns_percent\": " << dive.cns << ",\n";
	os << "    \"otu\": " << dive.otu << "\n";
	os << "  },\n";

	os << "  \"cylinders\": [\n";
	for (size_t i = 0; i < dive.cylinders.size(); i++) {
		const cylinder_t &c = *dive.get_cylinder(i);
		os << "    {\"index\": " << i << ", \"gas\": ";
		json_escape(os, c.gasmix.name());
		os << ", \"size_l\": " << c.type.size.mliter / 1000.0
		   << ", \"working_pressure_bar\": " << c.type.workingpressure.mbar / 1000.0
		   << ", \"start_bar\": " << c.start.mbar / 1000.0
		   << ", \"end_bar\": " << c.end.mbar / 1000.0
		   << ", \"gas_used_l\": " << c.gas_used.mliter / 1000.0
		   << ", \"deco_gas_used_l\": " << c.deco_gas_used.mliter / 1000.0
		   << ", \"use\": " << (int)c.cylinder_use
		   << "}";
		if (i + 1 < dive.cylinders.size())
			os << ",";
		os << "\n";
	}
	os << "  ],\n";

	os << "  \"deco_stops\": [";
	bool first_stop = true;
	for (const auto &s : res.stops) {
		if (s.time <= 0)
			continue; // transition depths passed on the way up
		os << (first_stop ? "" : ", ") << "{\"depth_m\": " << s.depth / 1000.0
		   << ", \"duration_s\": " << s.time << "}";
		first_stop = false;
	}
	os << "],\n";

	os << "  \"gas_switch_events\": [";
	bool first = true;
	for (const auto &ev : dc.events) {
		if (ev.type != SAMPLE_EVENT_GASCHANGE2 && ev.type != SAMPLE_EVENT_GASCHANGE)
			continue;
		os << (first ? "" : ", ") << "{\"time_s\": " << ev.time.seconds
		   << ", \"cylinder\": " << ev.gas.index << "}";
		first = false;
	}
	os << "],\n";

	os << "  \"tissues_final\": {\n";
	os << "    \"n2_sat_bar\": [";  for (int i = 0; i < 16; i++) os << (i ? ", " : "") << res.ds->tissue_n2_sat[i];  os << "],\n";
	os << "    \"he_sat_bar\": [";  for (int i = 0; i < 16; i++) os << (i ? ", " : "") << res.ds->tissue_he_sat[i];  os << "],\n";
	os << "    \"tolerated_bar\": [";  for (int i = 0; i < 16; i++) os << (i ? ", " : "") << res.ds->tolerated_by_tissue[i];  os << "]\n";
	os << "  },\n";

	os << "  \"samples\": [\n";
	for (size_t i = 0; i < pi.entry.size(); i++) {
		const auto &e = pi.entry[i];
		if (e.sec < 0)
			continue;
		char buf[256];
		os << "    {\"time_s\": " << e.sec << ", \"depth_m\": " << e.depth.mm / 1000.0
		   << ", \"ceiling_m\": " << e.ceiling.mm / 1000.0
		   << ", \"in_deco\": " << (e.in_deco ? "true" : "false")
		   << ", \"ndl_s\": " << e.ndl
		   << ", \"tts_s\": " << e.tts;
		snprintf(buf, sizeof(buf), ", \"gf\": %.2f, \"surface_gf\": %.2f, \"cns\": %d",
			 e.current_gf, e.surface_gf, e.cns);
		os << buf;
		snprintf(buf, sizeof(buf), ", \"po2_bar\": %.3f, \"pn2_bar\": %.3f, \"phe_bar\": %.3f, \"amb_bar\": %.3f",
			 e.pressures.o2, e.pressures.n2, e.pressures.he, e.ambpressure);
		os << buf;
		os << ", \"tissue_sat_percent\": [";
		for (int t = 0; t < 16; t++)
			os << (t ? ", " : "") << e.percentages[t];
		os << "], \"tissue_ceiling_m\": [";
		for (int t = 0; t < 16; t++)
			os << (t ? ", " : "") << e.ceilings[t].mm / 1000.0;
		os << "], \"cyl_pressures_bar\": [";
		for (size_t c = 0; c < dive.cylinders.size(); c++) {
			int p = get_plot_sensor_pressure(pi, i, c);
			if (!p)
				p = get_plot_interpolated_pressure(pi, i, c);
			os << (c ? ", " : "") << p / 1000.0;
		}
		os << "]}";
		if (i + 1 < pi.entry.size())
			os << ",";
		os << "\n";
	}
	os << "  ]\n}\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
	// force C locale so numeric output is stable (dots, not commas)
	setlocale(LC_NUMERIC, "C");

	// suppress Qt logging noise
	qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) {});
	QCoreApplication app(argc, argv);
	verbose = 1;

	bool opt_csv = false, opt_json = false;
	std::string outfile, planfile;

	QStringList args = app.arguments();
	for (int i = 1; i < args.size(); i++) {
		std::string a = args[i].toStdString();
		if (a == "--csv")
			opt_csv = true;
		else if (a == "--json")
			opt_json = true;
		else if (a == "--output")
			outfile = args[++i].toStdString();
		else if (a == "--help" || a == "-h") {
			printf("usage: planner-cli [--csv | --json] [--output FILE] <planfile> | -\n"
			       "\n"
			       "  --csv      per-sample profile data (comma separated)\n"
			       "  --json     machine readable full result\n"
			       "  --output   write output to FILE instead of stdout ('-' = stdout)\n"
			       "  planfile   dive plan in the planner-cli format ('-' = stdin)\n"
			       "\n"
			       "Documentation: subsurface/planner-cli/README.md\n");
			return 0;
		} else if (a == "-") {
			planfile = a;
		} else if (!a.empty() && a[0] == '-') {
			die("unknown option " + a);
		} else {
			planfile = a;
		}
	}
	if (planfile.empty())
		die("no plan file given (use '-' for stdin, --help for more)");

	git_libgit2_init();
	setup_system_prefs();
	// (re)force the C locale after all library init: numeric output must
	// stay stable regardless of the user's environment
	setlocale(LC_ALL, "C");

	std::ifstream file;
	std::istream *in = &std::cin;
	if (planfile == "-") {
		if (isatty(0))
			fprintf(stderr, "reading plan from stdin...\n");
	} else {
		file.open(planfile);
		if (!file.is_open())
			die("cannot open plan file " + planfile);
		in = &file;
	}

	PlanInput plan = parse_plan(*in);
	validate_plan(plan);

	Results res = run_plan(plan);

	if (res.error != PLAN_OK) {
		const char *msg = res.error == PLAN_ERROR_TIMEOUT ?
			"deco calculation aborted due to excessive time" :
			res.error == PLAN_ERROR_INAPPROPRIATE_GAS ?
			"a tank has a use type inappropriate for the dive mode" :
			res.error == PLAN_ERROR_NO_SUITABLE_BAILOUT_GAS ?
			"no suitable bailout gas found" : "planner error";
		fprintf(stderr, "planner-cli: planner warning: %s\n", msg);
	}

	std::ostream *out = &std::cout;
	std::ofstream outfile_stream;
	if (!outfile.empty() && outfile != "-") {
		outfile_stream.open(outfile);
		if (!outfile_stream.is_open())
			die("cannot open output file " + outfile);
		out = &outfile_stream;
	}

	if (opt_csv)
		print_csv(res, *out);
	else if (opt_json)
		print_json(plan, res, *out);
	else
		print_text(plan, res, *out);

	return res.error == PLAN_OK ? 0 : 2;
}
