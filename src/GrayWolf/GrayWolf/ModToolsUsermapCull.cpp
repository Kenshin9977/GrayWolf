#include "stdafx.h"

// The culling diagnostic a usermap gets with bo3_usermap_debug "cull" or "both" (ModToolsUsermap::SetDebug):
// scripts/zm/graywolf_cull_debug.gsc (server) and graywolf_cull_debug.csc (client), verbatim, in raw
// string pieces (MSVC takes 16 KB a literal at most). Every builtin is in docs_modtools'
// bo3_scriptapifunctions.htm; r_lodbiasrigid and r_modellodbias are dvar names boiii.exe holds.
// Use: graywolf-tools/docs/bo3-modtools-limits.md (Occlusion).

#include <string>

namespace ModToolsUsermap
{
namespace
{
    std::string Joined(const char* const* Pieces, size_t N)
    {
        std::string Out;
        for (size_t I = 0; I < N; I++) Out += Pieces[I];
        std::string Crlf;
        for (char C : Out) Crlf += C == '\n' ? std::string("\r\n") : std::string(1, C);
        return Crlf;
    }
}

std::string CullDebugScript()
{
    static const char* const Pieces[] = {
        R"GSC(// GrayWolf culling diagnostic (--bo3-usermap-debug cull), server part: written by GrayWolf, edits lost.
// "GWC run=<id> seq=<n> t=<ms> ev=<kind> key=value" console lines (+set logfile 2: console_mp.log).
// Hold F (Use), then: Sprint = eye-height sweep (again: stop; during it F alone marks a change),
// Reload = mark seen/hidden, Tactical = next test, Jump = twins. Console: set gwc_cmd <command>.

#using scripts\codescripts\struct;
#using scripts\shared\callbacks_shared;
#using scripts\shared\system_shared;
#using scripts\zm\graywolf_cull_models;

#insert scripts\shared\shared.gsh;

#namespace graywolf_cull_debug;

REGISTER_SYSTEM( "graywolf_cull_debug", &__init__, undefined )

function __init__()
{
	level.gwc_seq = 0;
	level.gwc_run = "" + RandomInt( 1000000 );
	level.gwc_client_seq = 0;
	level.gwc_umbra = 0;
	level.gwc_twin_mode = 0;
	level.gwc_twins = [];
	level.gwc_sweeping = false;
	level.gwc_sweep_h = 0;
	level.gwc_mark = 0;
	level.gwc_auto = false;
	SetDvar( "gwc_cmd", "" );
	SetDvar( "gwc_client", "" );
	init_dvar( "gwc_sweep_from", "8" );
	init_dvar( "gwc_sweep_to", "88" );
	init_dvar( "gwc_sweep_step", "2" );
	init_dvar( "gwc_sweep_wait", "1" );
	init_dvar( "gwc_radius", "2500" );
	init_dvar( "gwc_twin_max", "48" );
	callback::on_spawned( &on_player_spawned );
	level thread command_loop();
}

function init_dvar( name, value )
{
	if ( GetDvarString( name, "" ) == "" )
	{
		SetDvar( name, value );
	}
}

function gw_log( ev, kv, show = false )
{
	level.gwc_seq++;
	line = "GWC run=" + level.gwc_run + " seq=" + level.gwc_seq + " t=" + GetTime() + " ev=" + ev + " " + kv;
	PrintLn( line );
	if ( show )
	{
		IPrintLn( line );
	}
}

function n1( v )
{
	if ( !IsDefined( v ) )
	{
		return "u";
	}
	return "" + ( Floor( v * 10 + 0.5 ) / 10 );
}

function vec1( v )
{
	if ( !IsDefined( v ) )
	{
		return "u";
	}
	return n1( v[ 0 ] ) + "," + n1( v[ 1 ] ) + "," + n1( v[ 2 ] );
}

// a command for the client script: gwc_client = "<seq> <command> <args>" (one process: the client reads it)
function client( cmd )
{
	level.gwc_client_seq++;
	SetDvar( "gwc_client", level.gwc_client_seq + " " + cmd );
	gw_log( "toclient", "cmd=" + StrTok( cmd, " " )[ 0 ] + " line=" + level.gwc_client_seq );
}

// ---- the eye: camera, stance, what is over and under it; then the client's static models in view

function eye( why )
{
	e = self GetPlayerCameraPos();
	a = self GetPlayerAngles();
	p = self.origin;
	up = BulletTrace( e, e + ( 0, 0, 1024 ), false, self );
	dn = BulletTrace( e, e - ( 0, 0, 1024 ), false, self );
	kv = "why=" + why + " stance=" + self GetStance() + " feet=" + vec1( p ) + " eye=" + vec1( e ) + " eyeh=" + n1( e[ 2 ] - p[ 2 ] );
	kv = kv + " vh=" + n1( self GetPlayerViewHeight() ) + " pitch=" + n1( a[ 0 ] ) + " yaw=" + n1( a[ 1 ] ) + " og=" + self IsOnGround();
	kv = kv + " over=" + ( up[ "fraction" ] < 1 ? n1( up[ "position" ][ 2 ] - e[ 2 ] ) : "none" ) + " under=" + ( dn[ "fraction" ] < 1 ? n1( e[ 2 ] - dn[ "position" ][ 2 ] ) : "none" );
	kv = kv + " test=" + level.gwc_umbra + " twins=" + level.gwc_twin_mode + "/" + level.gwc_twins.size + " mark=" + level.gwc_mark;
	kv = kv + " lodbiasrigid=" + GetDvarString( "r_lodbiasrigid", "u" ) + " modellodbias=" + GetDvarString( "r_modellodbias", "u" );
	gw_log( "eye", kv, true );
	client( "near " + GetDvarInt( "gwc_radius", 2500 ) + " " + why );
}

// ---- eye-height sweep: a script camera over the feet, gwc_sweep_from to gwc_sweep_to units, a step every
// gwc_sweep_wait seconds; F alone (no Sprint) marks where the models vanish or come back

function sweep()
{
	self endon( "disconnect" );
	level endon( "gwc_sweep_stop" );
	level.gwc_sweeping = true;
	p = self.origin;
	a = self GetPlayerAngles();
	from = GetDvarFloat( "gwc_sweep_from", 8 );
	to = GetDvarFloat( "gwc_sweep_to", 88 );
	step = GetDvarFloat( "gwc_sweep_step", 2 );
	w = GetDvarFloat( "gwc_sweep_wait", 1 );
	if ( step < 0.5 )
	{
		step = 0.5;
	}
	gw_log( "sweep", "state=start feet=" + vec1( p ) + " pitch=" + n1( a[ 0 ] ) + " yaw=" + n1( a[ 1 ] ) + " from=" + from + " to=" + to + " step=" + step + " wait=" + w + " stance=" + self GetStance(), true );
	self thread sweep_end( p );
	self CameraSetPosition( p + ( 0, 0, from ), a );
	self CameraActivate( true );
	changes = 0;
	was = true;
	for ( h = from; h <= to; h += step )
	{
		level.gwc_sweep_h = h;
		self CameraSetPosition( p + ( 0, 0, h ), a );
		gw_log( "step", "h=" + n1( h ) + " z=" + n1( p[ 2 ] + h ) );
		t = 0;
		while ( t < w )
		{
			wait 0.05;
			t += 0.05;
			u = self UseButtonPressed() && !self SprintButtonPressed();
			if ( u && !was )
			{
				changes++;
				gw_log( "mark", "src=sweep h=" + n1( h ) + " z=" + n1( p[ 2 ] + h ) + " feet=" + vec1( p ) + " yaw=" + n1( a[ 1 ] ) + " change=" + changes, true );
			}
			was = u;
		}
	}
	level notify( "gwc_sweep_stop" );
}

function sweep_end( p )
{
	self endon( "disconnect" );
	level waittill( "gwc_sweep_stop" );
	self CameraActivate( false );
	level.gwc_sweeping = false;
	gw_log( "sweep", "state=end feet=" + vec1( p ) + " last_h=" + n1( level.gwc_sweep_h ), true );
}

// ---- tests, one more each time (Umbra's on the client stay until the level restarts)

function umbra_next( n )
{
	if ( !IsDefined( n ) )
	{
		n = level.gwc_umbra + 1;
	}
	if ( n > 7 || n < 0 )
	{
		n = 0;
	}
	level.gwc_umbra = n;
	what = array( "none", "umbra_contribution_0", "umbra_accurate_1e6", "umbra_accurate_0", "umbra_distance_100", "umbra_distance_0.01", "lodbias_-1000", "lodbias_0" );
	if ( n == 6 )
	{
		SetDvar( "r_lodbiasrigid", "-1000" );
		SetDvar( "r_modellodbias", "-1000" );
	}
	else if ( n == 7 )
	{
		SetDvar( "r_lodbiasrigid", "0" );
		SetDvar( "r_modellodbias", "0" );
	}
	gw_log( "test", "test=" + n + " what=" + what[ n ] + " lodbiasrigid=" + GetDvarString( "r_lodbiasrigid", "u" ) + " modellodbias=" + GetDvarString( "r_modellodbias", "u" ), true );
	IPrintLnBold( "GWC test " + n + ": " + what[ n ] );
	if ( n >= 1 && n <= 5 )
	{
		client( "umbra " + n );
	}
}

// twins: a script_model over each placed model in view (nearest first), gwc_twin_max at most; mode 2
// also SetForceNoCull. Twins that stay where the static models vanish: the static models' own culling.
function twins( mode )
{
	if ( !IsDefined( mode ) )
	{
		mode = ( level.gwc_twin_mode + 1 ) % 3;
	}
	level.gwc_twin_mode = mode;
	foreach ( e in level.gwc_twins )
	{
		if ( IsDefined( e ) )
		{
			e Delete();
		}
	}
	level.gwc_twins = [];
	if ( mode == 0 )
	{
		gw_log( "twins", "mode=0 count=0", true );
		return;
	}
	graywolf_cull_models::load();
	e0 = self GetPlayerCameraPos();
	f = AnglesToForward( ( 0, self GetPlayerAngles()[ 1 ], 0 ) );
	most = GetDvarInt( "gwc_twin_max", 48 );
	taken = [];
	lo = 0;
	foreach ( ring in array( 600, 1200, 2500, 6000 ) )
	{
		for ( i = 0; i < level.gwm_origins.size && level.gwc_twins.size < most; i++ )
		{
			d = level.gwm_origins[ i ] - e0;
			flat = ( d[ 0 ], d[ 1 ], 0 );
			dist = Length( flat );
			if ( dist > ring || dist <= lo || dist < 1 )
			{
				continue;
			}
			if ( VectorDot( VectorNormalize( flat ), f ) < 0.6 )
			{
				continue;
			}
			e = Spawn( "script_model", level.gwm_origins[ i ] );
			e.angles = level.gwm_angles[ i ];
			e SetModel( level.gwm_names[ i ] );
			if ( level.gwm_scales[ i ] != 1 )
			{
				e SetScale( level.gwm_scales[ i ] );
			}
			if ( mode == 2 )
			{
				e SetForceNoCull();
			}
			level.gwc_twins[ level.gwc_twins.size ] = e;
			gw_log( "twin", "i=" + i + " name=" + level.gwm_names[ i ] + " pos=" + vec1( level.gwm_origins[ i ] ) + " d=" + Int( dist ) + " nocull=" + ( mode == 2 ) );
		}
		lo = ring;
	}
	gw_log( "twins", "mode=" + mode + " count=" + level.gwc_twins.size + " eye=" + vec1( e0 ), true );
}
)GSC",
        R"GSC(
function mark( src )
{
	level.gwc_mark = 1 - level.gwc_mark;
	e = self GetPlayerCameraPos();
	gw_log( "mark", "src=" + src + " seen=" + ( level.gwc_mark ? "hidden" : "visible" ) + " stance=" + self GetStance() + " eye=" + vec1( e ) + " eyeh=" + n1( e[ 2 ] - self.origin[ 2 ] ) + " yaw=" + n1( self GetPlayerAngles()[ 1 ] ) + " test=" + level.gwc_umbra + " twins=" + level.gwc_twin_mode, true );
}

// ---- player

function on_player_spawned()
{
	self endon( "disconnect" );
	self notify( "gwc_spawned" );
	self endon( "gwc_spawned" );
	wait 1;
	gw_log( "spawn", "pos=" + vec1( self.origin ) + " models=" + graywolf_cull_models::count(), true );
	self thread key_loop();
	self thread stance_watch();
	self thread hud_loop();
}

function key_loop()
{
	self endon( "disconnect" );
	self endon( "gwc_spawned" );
	while ( true )
	{
		wait 0.05;
		if ( !self UseButtonPressed() )
		{
			continue;
		}
		if ( self SprintButtonPressed() )
		{
			if ( level.gwc_auto )
			{
				gw_log( "key", "key=F+Shift action=auto_stop", true );
				level notify( "gwc_auto_stop" );
			}
			else
			{
				gw_log( "key", "key=F+Shift action=auto_start", true );
				self thread auto();
			}
			wait 1;
		}
		else if ( level.gwc_auto )
		{
			continue;   // the automatic run reads F and F+R itself
		}
		else if ( self ReloadButtonPressed() )
		{
			gw_log( "key", "key=F+R action=mark", true );
			self mark( "key" );
			wait 0.6;
		}
		else if ( self SecondaryOffhandButtonPressed() )
		{
			gw_log( "key", "key=F+Tactical action=next_test", true );
			self umbra_next();
			wait 0.3;
			self eye( "test" );
			wait 0.7;
		}
		else if ( self JumpButtonPressed() )
		{
			gw_log( "key", "key=F+Jump action=twins", true );
			self twins();
			wait 1;
		}
	}
}

// ---- the automatic run at the player's spot (F+Shift): standing and crouched, asked whether the models
// show; an eye-height sweep (F marks each change); twins, then twins never culled; tests 1 to 6, each
// asked; one summary line. Umbra's tests stay until the level restarts.

function prompt( text )
{
	if ( IsDefined( self.gwc_prompt ) )
	{
		self.gwc_prompt SetText( text );
	}
	if ( text != "" )
	{
		IPrintLn( text );
	}
}

function ask( tag, text, secs )
{
	self prompt( text );
	for ( t = 0; t < secs; t += 0.05 )
	{
		wait 0.05;
		if ( self UseButtonPressed() && self ReloadButtonPressed() )
		{
			gw_log( "answer", "step=" + tag + " answer=yes stance=" + self GetStance(), true );
			self prompt( "OUI note" );
			wait 1;
			return "yes";
		}
	}
	gw_log( "answer", "step=" + tag + " answer=no stance=" + self GetStance(), true );
	return "no";
}

function auto()
{
	self endon( "disconnect" );
	level endon( "gwc_auto_stop" );
	level.gwc_auto = true;
	p = self.origin;
	a = self GetPlayerAngles();
	gw_log( "auto", "state=start feet=" + vec1( p ) + " yaw=" + n1( a[ 1 ] ), true );
	self thread auto_end();
	res = "";
	self SetStance( "stand" );
	wait 1.5;
	self eye( "auto_stand" );
	res = res + " stand=" + self ask( "stand", "DEBOUT: haie, lampadaire, chateau d'eau visibles ? F+R = OUI (sinon attendre)", 7 );
	self SetStance( "crouch" );
	wait 1.5;
	self eye( "auto_crouch" );
	res = res + " crouch=" + self ask( "crouch", "ACCROUPI: visibles ? F+R = OUI (sinon attendre)", 7 );
	self prompt( "BALAYAGE: la camera monte de +16 a +84. Appuie sur F seul a chaque apparition ou disparition" );
	wait 4;
	self CameraSetPosition( p + ( 0, 0, 16 ), a );
	self CameraActivate( true );
	changes = "";
	was = true;
	for ( h = 16; h <= 84; h += 2 )
	{
		level.gwc_sweep_h = h;
		self CameraSetPosition( p + ( 0, 0, h ), a );
		for ( t = 0; t < 0.6; t += 0.05 )
		{
			wait 0.05;
			u = self UseButtonPressed() && !self ReloadButtonPressed();
			if ( u && !was )
			{
				changes = changes + ( changes == "" ? "" : "," ) + h;
				gw_log( "mark", "src=auto_sweep h=" + h + " z=" + n1( p[ 2 ] + h ), true );
			}
			was = u;
		}
	}
	self CameraActivate( false );
	level.gwc_sweep_h = 0;
	res = res + " sweep_changes=" + ( changes == "" ? "none" : changes );
	self SetStance( "crouch" );
	wait 1;
	self twins( 1 );
	wait 1;
	res = res + " twins=" + self ask( "twins", "ACCROUPI + JUMEAUX (copies des modeles): visibles ? F+R = OUI", 7 );
	self twins( 2 );
	wait 1;
	res = res + " twins_nocull=" + self ask( "twins_nocull", "JUMEAUX jamais culles: visibles ? F+R = OUI", 7 );
	self twins( 0 );
	first = "none";
	for ( n = 1; n <= 6; n++ )
	{
		self SetStance( "crouch" );
		self umbra_next( n );
		wait 1.5;
		r = self ask( "test" + n, "TEST " + n + " (accroupi): objets revenus ? F+R = OUI", 7 );
		res = res + " test" + n + "=" + r;
		if ( r == "yes" && first == "none" )
		{
			first = "" + n;
		}
	}
	self umbra_next( 7 );
	res = res + " first_test_back=" + first;
	gw_log( "auto_summary", "feet=" + vec1( p ) + " yaw=" + n1( a[ 1 ] ) + res, true );
	IPrintLnBold( "GWC: fini. Relancer la carte avant un autre endroit." );
	level notify( "gwc_auto_stop" );
}

function auto_end()
{
	self endon( "disconnect" );
	level waittill( "gwc_auto_stop" );
	self CameraActivate( false );
	level.gwc_auto = false;
	self prompt( "" );
	gw_log( "auto", "state=end", true );
}

// each stance change: the eye half a second later (the camera has moved by then)
function stance_watch()
{
	self endon( "disconnect" );
	self endon( "gwc_spawned" );
	last = self GetStance();
	while ( true )
	{
		wait 0.05;
		st = self GetStance();
		if ( st != last && !level.gwc_sweeping && !level.gwc_auto )
		{
			wait 0.5;
			self eye( "stance" );
			st = self GetStance();
		}
		last = st;
	}
}

function hud_row( i, label )
{
	t = NewClientHudElem( self );
	t.alignX = "left";
	t.alignY = "top";
	t.horzAlign = "user_left";
	t.vertAlign = "user_top";
	t.x = 8;
	t.y = 60 + i * 13;
	t.fontScale = 1.1;
	t.foreground = true;
	t.hidewheninmenu = true;
	t SetText( label );
	v = NewClientHudElem( self );
	v.alignX = "left";
	v.alignY = "top";
	v.horzAlign = "user_left";
	v.vertAlign = "user_top";
	v.x = 120;
	v.y = 60 + i * 13;
	v.fontScale = 1.1;
	v.foreground = true;
	v.hidewheninmenu = true;
	v.color = ( 0.4, 1, 1 );
	v SetValue( 0 );
	return v;
}

function hud_loop()
{
	self endon( "disconnect" );
	self endon( "gwc_spawned" );
	labels = array( "GWC eye - feet", "eye z", "stance 0s1c2p", "sweep h", "test (F+Q)", "twins (F+Saut)", "mark (F+R)" );
	help = NewClientHudElem( self );
	help.alignX = "left";
	help.alignY = "top";
	help.horzAlign = "user_left";
	help.vertAlign = "user_top";
	help.x = 8;
	help.y = 44;
	help.fontScale = 1.1;
	help.foreground = true;
	help.hidewheninmenu = true;
	help.color = ( 1, 1, 0.4 );
	help SetText( "GWC  F+Maj: test automatique ici (encore: arret)  |  F+R: oui / marque  |  F+Q: test suivant  |  F+Saut: jumeaux" );
	self.gwc_prompt = NewClientHudElem( self );
	self.gwc_prompt.alignX = "center";
	self.gwc_prompt.alignY = "middle";
	self.gwc_prompt.horzAlign = "center";
	self.gwc_prompt.vertAlign = "middle";
	self.gwc_prompt.y = 60;
	self.gwc_prompt.fontScale = 1.6;
	self.gwc_prompt.foreground = true;
	self.gwc_prompt.color = ( 1, 1, 0.4 );
	self.gwc_prompt SetText( "" );
	rows = [];
	for ( i = 0; i < labels.size; i++ )
	{
		rows[ i ] = self hud_row( i, labels[ i ] );
	}
	while ( true )
	{
		wait 0.2;
		e = self GetPlayerCameraPos();
		st = self GetStance();
		rows[ 0 ] SetValue( Floor( ( e[ 2 ] - self.origin[ 2 ] ) * 10 ) / 10 );
		rows[ 1 ] SetValue( Floor( e[ 2 ] * 10 ) / 10 );
		rows[ 2 ] SetValue( ( st == "stand" ? 0 : ( st == "crouch" ? 1 : 2 ) ) );
		rows[ 3 ] SetValue( ( level.gwc_sweeping ? level.gwc_sweep_h : -1 ) );
		rows[ 4 ] SetValue( level.gwc_umbra );
		rows[ 5 ] SetValue( level.gwc_twin_mode );
		rows[ 6 ] SetValue( level.gwc_mark );
	}
}

// ---- console: set gwc_cmd eye|near [r]|sweep|stop|test [n]|twins [n]|blink <name part>|mark

function command_loop()
{
	while ( true )
	{
		wait 0.25;
		cmd = GetDvarString( "gwc_cmd", "" );
		if ( cmd == "" )
		{
			continue;
		}
		SetDvar( "gwc_cmd", "" );
		args = StrTok( cmd, " " );
		players = GetPlayers();
		if ( players.size == 0 || args.size == 0 )
		{
			continue;
		}
		player = players[ 0 ];
		gw_log( "cmd", "cmd=" + args[ 0 ] );
		switch ( args[ 0 ] )
		{
			case "eye":
				player eye( "cmd" );
				break;
			case "near":
				client( "near " + ( args.size > 1 ? args[ 1 ] : "" + GetDvarInt( "gwc_radius", 2500 ) ) + " cmd" );
				break;
			case "sweep":
				if ( !level.gwc_sweeping )
				{
					player thread sweep();
				}
				break;
			case "stop":
				level notify( "gwc_sweep_stop" );
				break;
			case "test":
				player umbra_next( ( args.size > 1 ? Int( args[ 1 ] ) : undefined ) );
				break;
			case "twins":
				player twins( ( args.size > 1 ? Int( args[ 1 ] ) : undefined ) );
				break;
			case "blink":
				if ( args.size > 1 )
				{
					client( "blink " + args[ 1 ] );
				}
				break;
			case "mark":
				player mark( "cmd" );
				break;
			default:
				IPrintLn( "GWC unknown command " + cmd );
				break;
		}
	}
}
)GSC",
    };
    return Joined(Pieces, sizeof(Pieces) / sizeof(Pieces[0]));
}

std::string CullDebugClientScript()
{
    static const char* const Pieces[] = {
        R"GSC(// GrayWolf culling diagnostic, client part: written by GrayWolf, edits lost. Reads gwc_client
// ("<seq> <command> <args>"), logs "GWC run=c ..." lines. umbra <1-5>: one Umbra parameter changed;
// near <radius> [why]: the placed models in view and the static model found at each origin;
// blink <name part>: those static models hidden and shown three times.

#using scripts\codescripts\struct;
#using scripts\shared\callbacks_shared;
#using scripts\shared\system_shared;
#using scripts\zm\graywolf_cull_models;

#insert scripts\shared\shared.gsh;

#namespace graywolf_cull_debug;

REGISTER_SYSTEM( "graywolf_cull_debug", &__init__, undefined )

function __init__()
{
	level.gwcc_seq = 0;
	level.gwcc_seen = "";
	level thread client_loop();
}

function cl_log( ev, kv )
{
	level.gwcc_seq++;
	PrintLn( "GWC run=c seq=" + level.gwcc_seq + " t=" + GetRealTime() + " ev=" + ev + " " + kv );
}

function n1( v )
{
	if ( !IsDefined( v ) )
	{
		return "u";
	}
	return "" + ( Floor( v * 10 + 0.5 ) / 10 );
}

function vec1( v )
{
	if ( !IsDefined( v ) )
	{
		return "u";
	}
	return n1( v[ 0 ] ) + "," + n1( v[ 1 ] ) + "," + n1( v[ 2 ] );
}

function client_loop()
{
	while ( true )
	{
		wait 0.1;
		s = GetDvarString( "gwc_client", "" );
		if ( s == "" || s == level.gwcc_seen )
		{
			continue;
		}
		level.gwcc_seen = s;
		a = StrTok( s, " " );
		if ( a.size < 2 )
		{
			continue;
		}
		switch ( a[ 1 ] )
		{
			case "umbra":
				umbra( 0, ( a.size > 2 ? Int( a[ 2 ] ) : 0 ) );
				break;
			case "near":
				near( 0, ( a.size > 2 ? Float( a[ 2 ] ) : 2500 ), ( a.size > 3 ? a[ 3 ] : "" ) );
				break;
			case "blink":
				if ( a.size > 2 )
				{
					level thread blink( 0, a[ 2 ] );
				}
				break;
		}
	}
}

function umbra( lcn, n )
{
	switch ( n )
	{
		case 1:
			Umbra_SetMinimumContributionThreshold( lcn, 0 );
			break;
		case 2:
			Umbra_SetAccurateOcclusionThreshold( lcn, 1000000 );
			break;
		case 3:
			Umbra_SetAccurateOcclusionThreshold( lcn, 0 );
			break;
		case 4:
			Umbra_SetDistanceScale( lcn, 100 );
			break;
		case 5:
			Umbra_SetDistanceScale( lcn, 0.01 );
			break;
	}
	cl_log( "umbra", "test=" + n + " cam=" + vec1( GetCamPosByLocalClientNum( lcn ) ) );
}

// the models the user watches: hedges, lights, poles, towers, fences
function suspect( name )
{
	foreach ( part in array( "hedge", "light", "pole", "tower", "water", "fence" ) )
	{
		if ( IsSubStr( name, part ) )
		{
			return true;
		}
	}
	return false;
}

// the k nearest of ids (distances ds)
function nearest( ids, ds, k )
{
	out = [];
	used = [];
	for ( j = 0; j < k && j < ids.size; j++ )
	{
		best = -1;
		for ( m = 0; m < ids.size; m++ )
		{
			if ( !IsDefined( used[ m ] ) && ( best < 0 || ds[ m ] < ds[ best ] ) )
			{
				best = m;
			}
		}
		used[ best ] = true;
		out[ out.size ] = ids[ best ];
	}
	return out;
}

// the placed models in view (55 degrees either side of the yaw, within r): the 12 nearest, and the 18
// nearest suspects (suspect()), with the static model found at each origin
function near( lcn, r, why )
{
	graywolf_cull_models::load();
	cam = GetCamPosByLocalClientNum( lcn );
	ang = GetCamAnglesByLocalClientNum( lcn );
	f = AnglesToForward( ( 0, ang[ 1 ], 0 ) );
	any_i = [];
	any_d = [];
	sus_i = [];
	sus_d = [];
	for ( i = 0; i < level.gwm_origins.size; i++ )
	{
		d = level.gwm_origins[ i ] - cam;
		flat = ( d[ 0 ], d[ 1 ], 0 );
		dist = Length( flat );
		if ( dist > r || dist < 1 || VectorDot( VectorNormalize( flat ), f ) < 0.57 )
		{
			continue;
		}
		if ( suspect( level.gwm_names[ i ] ) )
		{
			sus_i[ sus_i.size ] = i;
			sus_d[ sus_d.size ] = dist;
		}
		else
		{
			any_i[ any_i.size ] = i;
			any_d[ any_d.size ] = dist;
		}
	}
	none = 0;
	foreach ( kind in array( "suspect", "nearest" ) )
	{
		list = ( kind == "suspect" ? nearest( sus_i, sus_d, 18 ) : nearest( any_i, any_d, 12 ) );
		foreach ( i in list )
		{
			d = level.gwm_origins[ i ] - cam;
			idx = FindStaticModelIndex( level.gwm_origins[ i ] );
			w = "none";
			if ( IsDefined( idx ) )
			{
				w = "" + idx;
			}
			else
			{
				none++;
			}
			az = VectorToAngles( ( d[ 0 ], d[ 1 ], 0 ) )[ 1 ] - ang[ 1 ];
			cl_log( "near", "kind=" + kind + " i=" + i + " name=" + level.gwm_names[ i ] + " pos=" + vec1( level.gwm_origins[ i ] ) + " d=" + Int( Length( ( d[ 0 ], d[ 1 ], 0 ) ) ) + " az=" + n1( az ) + " pitch=" + n1( VectorToAngles( d )[ 0 ] ) + " smodel=" + w );
		}
	}
	cl_log( "nearsum", "why=" + why + " cam=" + vec1( cam ) + " pitch=" + n1( ang[ 0 ] ) + " yaw=" + n1( ang[ 1 ] ) + " r=" + Int( r ) + " in_view=" + ( any_i.size + sus_i.size ) + " suspects_in_view=" + sus_i.size + " no_smodel=" + none );
}

function blink( lcn, part )
{
	graywolf_cull_models::load();
	cam = GetCamPosByLocalClientNum( lcn );
	idx = [];
	for ( i = 0; i < level.gwm_origins.size && idx.size < 64; i++ )
	{
		if ( !IsSubStr( level.gwm_names[ i ], part ) || Distance( level.gwm_origins[ i ], cam ) > 6000 )
		{
			continue;
		}
		s = FindStaticModelIndex( level.gwm_origins[ i ] );
		if ( IsDefined( s ) )
		{
			idx[ idx.size ] = s;
		}
	}
	cl_log( "blink", "part=" + part + " smodels=" + idx.size );
	for ( k = 0; k < 3; k++ )
	{
		foreach ( s in idx )
		{
			HideStaticModel( s );
		}
		wait 0.5;
		foreach ( s in idx )
		{
			UnhideStaticModel( s );
		}
		wait 0.5;
	}
}
)GSC",
    };
    return Joined(Pieces, sizeof(Pieces) / sizeof(Pieces[0]));
}
}
