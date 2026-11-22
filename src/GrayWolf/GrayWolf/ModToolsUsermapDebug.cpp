#include "stdafx.h"

// The prone diagnostic a usermap gets with bo3_usermap_debug "prone" (ModToolsUsermap::SetDebug):
// scripts/zm/graywolf_prone_debug.gsc, verbatim (its own comments say what it does), in raw string
// pieces (MSVC takes 16 KB a literal at most)

#include <string>

namespace ModToolsUsermap
{
std::string ProneDebugScript()
{
    static const char* const Pieces[] = {
        R"GSC(// GrayWolf prone diagnostic (--bo3-usermap-debug prone): written by GrayWolf on each export, edits lost.
// Every record is one console line "GWP run=<id> seq=<n> t=<ms> ev=<kind> key=value ..." (PrintLn: the
// console and, with +set logfile 2, console_mp.log), the main ones also on screen (IPrintLn).
// Keys: Use+Melee a forced prone test here, Use+ADS the test over every spawn point (again: stop),
// Use+Grenade the test over a grid around the player. Console: set gwp_cmd probe|force|auto [from] [to]|
// grid|stop|summary|hud|allow|env (gwp_yaws, gwp_wait, gwp_grid_step, gwp_grid_radius, gwp_echo 0-2).
#using scripts\codescripts\struct;
#using scripts\shared\callbacks_shared;
#using scripts\shared\system_shared;
#using scripts\zm\graywolf_prone_points;

#insert scripts\shared\shared.gsh;

#namespace graywolf_prone_debug;

REGISTER_SYSTEM( "graywolf_prone_debug", &__init__, undefined )

function __init__()
{
	level.gwp_seq = 0;
	level.gwp_run = "" + RandomInt( 1000000 );
	level.gwp_q = [];
	level.gwp_q_dropped = 0;
	level.gwp_busy = false;
	level.gwp_hud_on = true;
	level.gwp_allow = false;
	reset_stats( "none" );
	SetDvar( "gwp_cmd", "" );
	init_dvar( "gwp_echo", "1" );
	init_dvar( "gwp_yaws", "0 90" );
	init_dvar( "gwp_wait", "0.8" );
	init_dvar( "gwp_grid_step", "64" );
	init_dvar( "gwp_grid_radius", "256" );
	callback::on_spawned( &on_player_spawned );
	level thread echo_loop();
	level thread command_loop();
}

function init_dvar( name, value )
{
	if ( GetDvarString( name, "" ) == "" )
	{
		SetDvar( name, value );
	}
}

//*****************************************************************************
// Output
//*****************************************************************************

function gw_log( ev, kv, show = false )
{
	level.gwp_seq++;
	line = "GWP run=" + level.gwp_run + " seq=" + level.gwp_seq + " t=" + GetTime() + " ev=" + ev + " " + kv;
	PrintLn( line );
	echo_level = GetDvarInt( "gwp_echo", 1 );
	if ( echo_level >= 2 || ( show && echo_level >= 1 ) )
	{
		gw_say( line );
	}
}

// on screen, a line every tenth of a second (a burst of IPrintLn overflows the client's command buffer)
function gw_say( text )
{
	if ( level.gwp_q.size < 64 )
	{
		level.gwp_q[ level.gwp_q.size ] = text;
	}
	else
	{
		level.gwp_q_dropped++;
	}
}

function echo_loop()
{
	while ( true )
	{
		if ( level.gwp_q.size > 0 )
		{
			text = level.gwp_q[ 0 ];
			ArrayRemoveIndex( level.gwp_q, 0 );
			IPrintLn( text );
		}
		wait 0.1;
	}
}

function n0( v )
{
	if ( !IsDefined( v ) )
	{
		return "u";
	}
	return "" + Int( Floor( v + 0.5 ) );
}

function n1( v )
{
	if ( !IsDefined( v ) )
	{
		return "u";
	}
	return "" + ( Floor( v * 10 + 0.5 ) / 10 );
}

function n3( v )
{
	if ( !IsDefined( v ) )
	{
		return "u";
	}
	return "" + ( Floor( v * 1000 + 0.5 ) / 1000 );
}

function vec1( v )
{
	if ( !IsDefined( v ) )
	{
		return "u";
	}
	return n1( v[ 0 ] ) + "," + n1( v[ 1 ] ) + "," + n1( v[ 2 ] );
}

// a vector's z, undefined (vz) or 0 (vz0) when there is no vector
function vz( v )
{
	if ( !IsDefined( v ) )
	{
		return undefined;
	}
	return v[ 2 ];
}

function vz0( v )
{
	if ( !IsDefined( v ) )
	{
		return 0;
	}
	return v[ 2 ];
}

// a value without spaces
function word( s )
{
	if ( !IsDefined( s ) )
	{
		return "u";
	}
	s = "" + s;
	parts = StrTok( s, " " );
	if ( parts.size == 0 )
	{
		return "-";
	}
	out = parts[ 0 ];
	for ( i = 1; i < parts.size; i++ )
	{
		out = out + "_" + parts[ i ];
	}
	return out;
}

function ent_desc( e )
{
	if ( !IsDefined( e ) )
	{
		return "none";
	}
	num = e GetEntityNumber();
	s = "" + num;
	if ( IsDefined( e.classname ) )
	{
		s = s + ":" + word( e.classname );
	}
	if ( IsDefined( e.targetname ) )
	{
		s = s + ":" + word( e.targetname );
	}
	if ( IsDefined( e.script_noteworthy ) )
	{
		s = s + ":" + word( e.script_noteworthy );
	}
	if ( IsDefined( e.model ) && e.model != "" )
	{
		s = s + ":" + word( e.model );
	}
	return s;
}

function stance_code( st )
{
	if ( st == "prone" )
	{
		return 2;
	}
	if ( st == "crouch" )
	{
		return 1;
	}
	return 0;
}

//*****************************************************************************
// Traces
//*****************************************************************************

// what stops a player (PHYSICS_TRACE_MASK_PLAYER), a box of half width r and height h swept a to b
function ptrace( a, b, r, h, ign )
{
	return PhysicsTrace( a, b, ( 0 - r, 0 - r, 0 ), ( r, r, h ), ign, PHYSICS_TRACE_MASK_PLAYER );
}

// "dz:nz:surf:frac" of the ground under q (24 above to 48 below), by the player mask; dz from z0
function ground_word( q, z0, r, ign )
{
	tr = ptrace( q + ( 0, 0, 24 ), q - ( 0, 0, 48 ), r, 0, ign );
	if ( tr[ "fraction" ] >= 1 )
	{
		return "none";
	}
	if ( tr[ "fraction" ] <= 0 )
	{
		return "solid";
	}
	return n1( tr[ "position" ][ 2 ] - z0 ) + ":" + n3( vz( tr[ "normal" ] ) ) + ":" + word( tr[ "surfacetype" ] );
}

// the same by a bullet (drawn surfaces and clips that stop bullets, no player clip)
function bullet_word( q, z0, ign )
{
	tr = BulletTrace( q + ( 0, 0, 24 ), q - ( 0, 0, 48 ), false, ign );
	if ( tr[ "fraction" ] >= 1 )
	{
		return "none";
	}
	return n1( tr[ "position" ][ 2 ] - z0 ) + ":" + n3( vz( tr[ "normal" ] ) ) + ":" + word( tr[ "surfacetype" ] );
}

// how far a box (half width r, height h) at height z over p goes towards dir, up to len
function clear_dist( p, dir, z, r, h, len, ign )
{
	a = p + ( 0, 0, z );
	tr = ptrace( a, a + dir * len, r, h, ign );
	return tr[ "fraction" ] * len;
}

function clear_word( p, dir, z, r, h, len, ign )
{
	a = p + ( 0, 0, z );
	tr = ptrace( a, a + dir * len, r, h, ign );
	if ( tr[ "fraction" ] >= 1 )
	{
		return n0( len );
	}
	s = n0( tr[ "fraction" ] * len ) + ":" + word( tr[ "surfacetype" ] );
	if ( IsDefined( tr[ "entity" ] ) )
	{
		num = tr[ "entity" ] GetEntityNumber();
		s = s + ":e" + num;
	}
	return s;
}

function keys_word( a )
{
	if ( !IsDefined( a ) )
	{
		return "u";
	}
	keys = GetArrayKeys( a );
	s = "";
	foreach ( k in keys )
	{
		s = s + ( s == "" ? "" : "," ) + k;
	}
	return s;
}

//*****************************************************************************
// Measures
//*****************************************************************************

// the triggers and volumes the player touches
function touching_word()
{
	if ( !IsDefined( level.gwp_volumes ) )
	{
		level.gwp_volumes = [];
		foreach ( c in array( "trigger_multiple", "trigger_radius", "trigger_box", "trigger_once", "trigger_hurt", "trigger_use", "trigger_use_touch", "info_volume" ) )
		{
			foreach ( e in GetEntArray( c, "classname" ) )
			{
				level.gwp_volumes[ level.gwp_volumes.size ] = e;
			}
		}
	}
	s = "";
	n = 0;
	foreach ( e in level.gwp_volumes )
	{
		if ( IsDefined( e ) && self IsTouching( e ) )
		{
			n++;
			if ( n <= 4 )
			{
				s = s + ( s == "" ? "" : "|" ) + ent_desc( e );
			}
		}
	}
	return "" + n + ( s == "" ? "" : "/" + s );
}

// the ground under p: player mask (point and hull), bullet, the ground entity, what the player touches
function probe_ground( p, idx, src )
{
	tr = ptrace( p + ( 0, 0, 24 ), p - ( 0, 0, 48 ), 0, 0, self );
	hull = PlayerPhysicsTrace( p + ( 0, 0, 24 ), p - ( 0, 0, 48 ) );
	bt = BulletTrace( p + ( 0, 0, 24 ), p - ( 0, 0, 48 ), false, self );
	up = ptrace( p + ( 0, 0, 2 ), p + ( 0, 0, 96 ), 0, 0, self );
	ge = self GetGroundEnt();
	kv = "i=" + idx + " src=" + src + " pos=" + vec1( p ) + " og=" + self IsOnGround();
	kv = kv + " pz=" + ( tr[ "fraction" ] < 1 ? n1( tr[ "position" ][ 2 ] - p[ 2 ] ) : "none" ) + " pnz=" + n3( vz( tr[ "normal" ] ) );
	kv = kv + " psurf=" + word( tr[ "surfacetype" ] ) + " pwalk=" + word( tr[ "walkable" ] ) + " pent=" + ent_desc( tr[ "entity" ] );
	kv = kv + " hz=" + n1( hull[ 2 ] - p[ 2 ] );
	kv = kv + " bz=" + ( bt[ "fraction" ] < 1 ? n1( bt[ "position" ][ 2 ] - p[ 2 ] ) : "none" ) + " bnz=" + n3( vz( bt[ "normal" ] ) );
	kv = kv + " bsurf=" + word( bt[ "surfacetype" ] ) + " bent=" + ent_desc( bt[ "entity" ] );
	kv = kv + " gent=" + ent_desc( ge ) + " ceil=" + n0( up[ "fraction" ] * 94 ) + " touch=" + self touching_word();
	gw_log( "ground", kv );
	level.gwp_last_surf = word( tr[ "surfacetype" ] );
	level.gwp_last_nz = vz( tr[ "normal" ] );
}

// eight directions from yaw0 (0 ahead, 2 left, 4 behind, 6 right): how far boxes go at prone heights,
// the ground 16, 32 and 48 ahead (player mask point and 4-unit box, bullet)
function probe_dirs( p, yaw0, idx )
{
	for ( d = 0; d < 8; d++ )
	{
		yaw = yaw0 + d * 45;
		dir = AnglesToForward( ( 0, yaw, 0 ) );
		kv = "i=" + idx + " d=" + d + " yaw=" + n0( yaw );
		kv = kv + " c2=" + clear_word( p, dir, 2, 2, 2, 64, self ) + " c8=" + clear_word( p, dir, 8, 2, 2, 64, self );
		kv = kv + " c14=" + clear_word( p, dir, 14, 2, 2, 64, self ) + " c20=" + clear_word( p, dir, 20, 2, 2, 64, self );
		kv = kv + " c28=" + clear_word( p, dir, 28, 2, 2, 64, self );
		kv = kv + " box=" + clear_word( p, dir, 1, 15, 30, 64, self );
		for ( k = 1; k <= 3; k++ )
		{
			q = p + dir * ( 16 * k );
			kv = kv + " g" + ( 16 * k ) + "=" + ground_word( q, p[ 2 ], 0, self ) + " gb" + ( 16 * k ) + "=" + ground_word( q, p[ 2 ], 4, self );
			kv = kv + " b" + ( 16 * k ) + "=" + bullet_word( q, p[ 2 ], self );
		}
		gw_log( "dir", kv );
	}
}

// stand, crouch, then prone at the player's place facing yaw; stance and view height after each.
// When prone fails, again with AllowProne( true ) first (a script may have turned prone off).
function try_prone( p, yaw, idx, src, teleport )
{
	if ( teleport )
	{
		self SetOrigin( p );
		self SetVelocity( ( 0, 0, 0 ) );
		self SetPlayerAngles( ( 0, yaw, 0 ) );
	}
	self SetStance( "stand" );
	wait 0.3;
	og = self IsOnGround();
	st0 = self GetStance();
	start = self.origin;
	self SetStance( "crouch" );
	wait 0.35;
	stc = self GetStance();
	self SetStance( "prone" );
	wait GetDvarFloat( "gwp_wait", 0.8 );
	stp = self GetStance();
	vh = self GetPlayerViewHeight();
	ok = ( stp == "prone" );
	ok2 = "-";
	if ( !ok )
	{
		self AllowProne( true );
		self SetStance( "prone" );
		wait GetDvarFloat( "gwp_wait", 0.8 );
		ok2 = "" + ( self GetStance() == "prone" );
	}
	mv = self GetNormalizedMovement();
	moved = Distance( self.origin, start );
	// more than 8 units down: the player fell through the floor, no prone result
	fell = self.origin[ 2 ] < start[ 2 ] - 8;
	kv = "i=" + idx + " src=" + src + " yaw=" + n0( yaw ) + " og=" + og + " st0=" + st0 + " stc=" + stc + " stp=" + stp;
	kv = kv + " ok=" + ok + " ok_allow=" + ok2 + " vh=" + n1( vh ) + " moved=" + n1( moved ) + " input=" + n1( Abs( mv[ 0 ] ) + Abs( mv[ 1 ] ) );
	kv = kv + " pos=" + vec1( start ) + " surf=" + level.gwp_last_surf + " fell=" + fell + " end=" + vec1( self.origin );
	gw_log( "try", kv, src != "auto" && src != "grid" );
	if ( fell )
	{
		level.gwp_fell++;
		ok = false;
	}
	else
	{
		tally( ok, stc == "crouch", ok2 == "1", level.gwp_last_surf, level.gwp_last_nz );
	}
	self SetStance( "stand" );
	wait 0.5;
	return ok;
}

//*****************************************************************************
// Statistics
//*****************************************************************************

function reset_stats( mode )
{
	level.gwp_mode = mode;
	level.gwp_n = 0;
	level.gwp_ok = 0;
	level.gwp_crouch_ok = 0;
	level.gwp_allow_ok = 0;
	level.gwp_points = 0;
	level.gwp_points_ok = 0;
	level.gwp_skipped = 0;
	level.gwp_skip_why = [];
	level.gwp_fell = 0;
	level.gwp_surf_n = [];
	level.gwp_surf_ok = [];
	level.gwp_nz_n = [];
	level.gwp_nz_ok = [];
	level.gwp_started = GetTime();
}

function tally( ok, crouch_ok, allow_ok, surf, nz )
{
	level.gwp_n++;
	if ( ok )
	{
		level.gwp_ok++;
	}
	if ( crouch_ok )
	{
		level.gwp_crouch_ok++;
	}
	if ( allow_ok )
	{
		level.gwp_allow_ok++;
	}
	if ( !IsDefined( surf ) )
	{
		surf = "u";
	}
	if ( !IsDefined( level.gwp_surf_n[ surf ] ) )
	{
		level.gwp_surf_n[ surf ] = 0;
		level.gwp_surf_ok[ surf ] = 0;
	}
	level.gwp_surf_n[ surf ]++;
	if ( ok )
	{
		level.gwp_surf_ok[ surf ]++;
	}
	bucket = "u";
	if ( IsDefined( nz ) )
	{
		bucket = "nz" + ( Floor( nz * 20 ) / 20 );
	}
	if ( !IsDefined( level.gwp_nz_n[ bucket ] ) )
	{
		level.gwp_nz_n[ bucket ] = 0;
		level.gwp_nz_ok[ bucket ] = 0;
	}
	level.gwp_nz_n[ bucket ]++;
	if ( ok )
	{
		level.gwp_nz_ok[ bucket ]++;
	}
}

function summary()
{
	rate = 0;
	if ( level.gwp_n > 0 )
	{
)GSC",
        R"GSC(		rate = Floor( 1000 * level.gwp_ok / level.gwp_n ) / 10;
	}
	kv = "mode=" + level.gwp_mode + " points=" + level.gwp_points + " points_ok=" + level.gwp_points_ok + " skipped=" + level.gwp_skipped;
	kv = kv + " tries=" + level.gwp_n + " ok=" + level.gwp_ok + " rate=" + rate + " crouch_ok=" + level.gwp_crouch_ok + " ok_after_allow=" + level.gwp_allow_ok;
	kv = kv + " fell=" + level.gwp_fell;
	foreach ( k in GetArrayKeys( level.gwp_skip_why ) )
	{
		kv = kv + " skip_" + k + "=" + level.gwp_skip_why[ k ];
	}
	kv = kv + " secs=" + Int( ( GetTime() - level.gwp_started ) / 1000 ) + " dropped_lines=" + level.gwp_q_dropped;
	gw_log( "summary", kv, true );
	foreach ( k in GetArrayKeys( level.gwp_surf_n ) )
	{
		gw_log( "sumsurf", "surf=" + k + " tries=" + level.gwp_surf_n[ k ] + " ok=" + level.gwp_surf_ok[ k ], true );
	}
	foreach ( k in GetArrayKeys( level.gwp_nz_n ) )
	{
		gw_log( "sumnz", "nz=" + k + " tries=" + level.gwp_nz_n[ k ] + " ok=" + level.gwp_nz_ok[ k ] );
	}
	IPrintLnBold( "GWP prone: " + level.gwp_ok + "/" + level.gwp_n + " ok (" + rate + "%), crouch " + level.gwp_crouch_ok + ", after AllowProne " + level.gwp_allow_ok );
}

//*****************************************************************************
// Tests
//*****************************************************************************

// a point left out of the attempts, counted by why (the summary's skip_<why>)
function skip_point( why, kv )
{
	level.gwp_skipped++;
	if ( !IsDefined( level.gwp_skip_why[ why ] ) )
	{
		level.gwp_skip_why[ why ] = 0;
	}
	level.gwp_skip_why[ why ]++;
	gw_log( "skip", kv + " why=" + why );
}

// "" when the player put at g came to rest at q on a floor: else why not ("fell": off the ground or
// more than 8 units down; "moved": pushed away; "floor_unseen": no floor a trace from above meets
// within 8 units under q, the player held by one only its own movement sees, or under the floor)
function placed_word( g, q )
{
	if ( !self IsOnGround() || q[ 2 ] < g[ 2 ] - 8 )
	{
		return "fell";
	}
	if ( Distance( q, g ) > 12 )
	{
		return "moved";
	}
	dn = ptrace( q + ( 0, 0, 24 ), q - ( 0, 0, 8 ), 0, 0, self );
	if ( dn[ "fraction" ] >= 1 )
	{
		return "floor_unseen";
	}
	return "";
}

// one point: dropped to the floor with the player's hull, the ground and the eight directions
// measured, then a prone attempt facing each of gwp_yaws
function test_point( p, idx, src )
{
	top = p + ( 0, 0, 40 );
	bottom = p - ( 0, 0, 64 );
	g = PlayerPhysicsTrace( top, bottom );
	if ( Distance( g, top ) < 0.5 )
	{
		skip_point( "hull_starts_in_solid", "i=" + idx + " src=" + src + " pos=" + vec1( p ) );
		return;
	}
	if ( Distance( g, bottom ) < 0.5 )
	{
		// nothing under the point a player's hull lands on, seen from above (no floor, or one facing down)
		skip_point( "no_floor_from_above", "i=" + idx + " src=" + src + " pos=" + vec1( p ) );
		return;
	}
	self SetOrigin( g + ( 0, 0, 2 ) );
	self SetVelocity( ( 0, 0, 0 ) );
	self SetStance( "stand" );
	wait 0.4;
	// where the player came to rest: on a floor a trace from above sees, near where it was put;
	// otherwise the point is no prone test (a fall, a floor only the player's own movement holds)
	q = self.origin;
	why = placed_word( g, q );
	if ( why != "" )
	{
		skip_point( why, "i=" + idx + " src=" + src + " pos=" + vec1( g ) + " rest=" + vec1( q ) + " og=" + self IsOnGround() );
		return;
	}
	self probe_ground( q, idx, src );
	self probe_dirs( q, 0, idx );
	level.gwp_points++;
	any_ok = false;
	foreach ( y in StrTok( GetDvarString( "gwp_yaws", "0 90" ), " " ) )
	{
		if ( self try_prone( q, Int( y ), idx, src, true ) )
		{
			any_ok = true;
		}
	}
	if ( any_ok )
	{
		level.gwp_points_ok++;
	}
}

function run_points( from, to )
{
	self endon( "disconnect" );
	level endon( "gwp_stop" );
	pts = graywolf_prone_points::get();
	kinds = graywolf_prone_points::kinds();
	if ( !IsDefined( from ) || from < 0 )
	{
		from = 0;
	}
	if ( !IsDefined( to ) || to <= 0 || to > pts.size )
	{
		to = pts.size;
	}
	level.gwp_busy = true;
	reset_stats( "auto" );
	yaws = StrTok( GetDvarString( "gwp_yaws", "0 90" ), " " );
	back = self.origin;
	backa = self GetPlayerAngles();
	self prepare( true );
	gw_log( "run", "mode=auto from=" + from + " to=" + to + " points=" + pts.size + " yaws=" + word( GetDvarString( "gwp_yaws", "0 90" ) ) + " eta_s=" + Int( ( to - from ) * ( 0.4 + yaws.size * ( 1.5 + GetDvarFloat( "gwp_wait", 0.8 ) ) ) ), true );
	self thread finish_on_stop( back, backa );
	for ( i = from; i < to; i++ )
	{
		self test_point( pts[ i ], i, "auto:" + kinds[ i ] );
		if ( ( i - from ) % 10 == 9 )
		{
			gw_say( "GWP auto " + ( i + 1 - from ) + "/" + ( to - from ) + " ok " + level.gwp_ok + "/" + level.gwp_n );
		}
	}
	level notify( "gwp_stop" );
}

function run_grid()
{
	self endon( "disconnect" );
	level endon( "gwp_stop" );
	level.gwp_busy = true;
	reset_stats( "grid" );
	step = GetDvarInt( "gwp_grid_step", 64 );
	rad = GetDvarInt( "gwp_grid_radius", 256 );
	if ( step < 8 )
	{
		step = 8;
	}
	c = self.origin;
	back = self.origin;
	backa = self GetPlayerAngles();
	self prepare( true );
	gw_log( "run", "mode=grid center=" + vec1( c ) + " step=" + step + " radius=" + rad + " yaws=" + word( GetDvarString( "gwp_yaws", "0 90" ) ), true );
	self thread finish_on_stop( back, backa );
	n = 0;
	for ( x = 0 - rad; x <= rad; x += step )
	{
		for ( y = 0 - rad; y <= rad; y += step )
		{
			q = c + ( x, y, 0 );
			top = q + ( 0, 0, 64 );
			g = PlayerPhysicsTrace( top, q - ( 0, 0, 192 ) );
			if ( Distance( g, top ) < 0.5 )
			{
				skip_point( "hull_starts_in_solid", "i=g" + n + " src=grid pos=" + vec1( q ) );
			}
			else
			{
				self test_point( g, "g" + n, "grid" );
			}
			n++;
		}
	}
	level notify( "gwp_stop" );
}

// the player out of the zombies' way while a run moves it
function prepare( on )
{
	if ( on )
	{
		self EnableInvulnerability();
		self.ignoreme = true;
	}
	else
	{
		self.ignoreme = false;
	}
}

function finish_on_stop( back, backa )
{
	self endon( "disconnect" );
	level waittill( "gwp_stop" );
	summary();
	self SetStance( "stand" );
	self SetOrigin( back );
	self SetPlayerAngles( backa );
	self prepare( false );
	level.gwp_busy = false;
}

// a prone attempt where the player stands, facing where it looks, with every measure
function manual_force()
{
	if ( level.gwp_busy )
	{
		return;
	}
	level.gwp_busy = true;
	idx = "m" + level.gwp_seq;
	p = self.origin;
	yaw = self GetPlayerAngles()[ 1 ];
	self probe_ground( p, idx, "manual" );
	self probe_dirs( p, yaw, idx );
	self try_prone( p, yaw, idx, "manual", false );
	level.gwp_busy = false;
}

// the measures only, no stance change
function manual_probe()
{
	idx = "p" + level.gwp_seq;
	p = self.origin;
	self probe_ground( p, idx, "probe" );
	self probe_dirs( p, self GetPlayerAngles()[ 1 ], idx );
	gw_say( "GWP probe " + idx + " surf=" + level.gwp_last_surf + " nz=" + n3( level.gwp_last_nz ) + " stance=" + self GetStance() );
}

function env()
{
	kv = "map=" + word( GetDvarString( "mapname", "?" ) ) + " gametype=" + word( GetDvarString( "g_gametype", "?" ) );
	kv = kv + " developer=" + word( GetDvarString( "developer", "?" ) ) + " logfile=" + word( GetDvarString( "logfile", "?" ) );
	kv = kv + " points=" + graywolf_prone_points::get().size + " yaws=" + word( GetDvarString( "gwp_yaws", "0 90" ) ) + " wait=" + GetDvarFloat( "gwp_wait", 0.8 );
	foreach ( c in array( "trigger_multiple", "trigger_radius", "trigger_box", "trigger_once", "trigger_hurt", "trigger_use", "info_volume", "script_brushmodel", "script_model" ) )
	{
		kv = kv + " n_" + c + "=" + GetEntArray( c, "classname" ).size;
	}
	gw_log( "env", kv, true );
	// the fields each trace gives back (once)
	p = self.origin;
	gw_log( "keys", "physicstrace=" + keys_word( ptrace( p + ( 0, 0, 24 ), p - ( 0, 0, 48 ), 0, 0, self ) ) );
	gw_log( "keys", "bullettrace=" + keys_word( BulletTrace( p + ( 0, 0, 24 ), p - ( 0, 0, 48 ), false, self ) ) );
	gw_log( "keys", "groundtrace=" + keys_word( GroundTrace( p + ( 0, 0, 24 ), p - ( 0, 0, 48 ), false, self ) ) );
	gw_log( "keys", "physicstraceex=" + keys_word( PhysicsTraceEx( p + ( 0, 0, 24 ), p - ( 0, 0, 48 ), ( 0, 0, 0 ), ( 0, 0, 0 ), self, true ) ) );
}

//*****************************************************************************
// Player
//*****************************************************************************

function on_player_spawned()
{
	self endon( "disconnect" );
	self notify( "gwp_spawned" );
	self endon( "gwp_spawned" );
	wait 1;
	gw_log( "spawn", "pos=" + vec1( self.origin ) + " stance=" + self GetStance() + " vh=" + n1( self GetPlayerViewHeight() ) + " noclip=" + self IsInMoveMode( "ufo", "noclip" ), true );
	self env();
	self thread hud_loop();
	self thread key_loop();
	self thread stance_watch();
	self thread allow_loop();
}

function key_loop()
{
	self endon( "disconnect" );
	self endon( "gwp_spawned" );
	while ( true )
	{
		WAIT_SERVER_FRAME;
		if ( !self UseButtonPressed() )
		{
			continue;
		}
		if ( self MeleeButtonPressed() )
		{
			self thread manual_force();
			wait 1;
		}
		else if ( self AdsButtonPressed() )
		{
			if ( level.gwp_busy )
			{
				level notify( "gwp_stop" );
			}
			else
			{
				self thread run_points( 0, 0 );
			}
			wait 1;
		}
		else if ( self FragButtonPressed() )
		{
			if ( level.gwp_busy )
			{
				level notify( "gwp_stop" );
			}
			else
			{
				self thread run_grid();
			}
			wait 1;
		}
	}
}

// every stance change, and what the stance button did 0.8 s after it was pressed
function stance_watch()
{
	self endon( "disconnect" );
	self endon( "gwp_spawned" );
	last = self GetStance();
	was = false;
	while ( true )
	{
		WAIT_SERVER_FRAME;
		st = self GetStance();
		if ( st != last && !level.gwp_busy )
		{
			gw_log( "stance", "from=" + last + " to=" + st + " pos=" + vec1( self.origin ) + " og=" + self IsOnGround() );
		}
		last = st;
		pressed = self StanceButtonPressed();
		if ( pressed && !was && !level.gwp_busy )
		{
			self thread stance_press();
		}
		was = pressed;
	}
}

function stance_press()
{
	st0 = self GetStance();
	p = self.origin;
	wait 0.8;
	yaw = self GetPlayerAngles()[ 1 ];
	f = AnglesToForward( ( 0, yaw, 0 ) );
	kv = "st0=" + st0 + " st1=" + self GetStance() + " pos=" + vec1( p ) + " yaw=" + n0( yaw ) + " og=" + self IsOnGround();
	kv = kv + " c14f=" + clear_word( p, f, 14, 2, 2, 64, self ) + " c14b=" + clear_word( p, f * -1, 14, 2, 2, 64, self );
	kv = kv + " g32f=" + ground_word( p + f * 32, p[ 2 ], 0, self ) + " g32b=" + ground_word( p - f * 32, p[ 2 ], 0, self );
	gw_log( "stancebtn", kv, true );
}

// gwp_cmd allow: AllowProne( true ) every frame, against a script that turns it off
function allow_loop()
{
	self endon( "disconnect" );
	self endon( "gwp_spawned" );
	while ( true )
	{
		WAIT_SERVER_FRAME;
		if ( level.gwp_allow )
		{
			self AllowProne( true );
		}
	}
}

//*****************************************************************************
// HUD: values only (SetValue), the labels set once
//*****************************************************************************

function hud_row( i, label )
{
	t = NewClientHudElem( self );
	t.alignX = "left";
	t.alignY = "top";
	t.horzAlign = "user_left";
	t.vertAlign = "user_top";
	t.x = 8;
	t.y = 120 + i * 13;
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
	v.y = 120 + i * 13;
	v.fontScale = 1.1;
	v.foreground = true;
	v.hidewheninmenu = true;
	v.color = ( 1, 1, 0.4 );
	v SetValue( 0 );
	self.gwp_labels[ self.gwp_labels.size ] = t;
	return v;
}

function hud_loop()
{
	self endon( "disconnect" );
	self endon( "gwp_spawned" );
	self.gwp_labels = [];
	labels = array( "GWP stance 0s1c2p", "on ground", "ground nz x1000", "clip-bullet dz", "clear fwd @14", "clear back @14", "clear left @14", "clear right @14", "ground fwd32 dz", "ground back32 dz", "triggers touched", "auto tries", "auto ok" );
	rows = [];
	for ( i = 0; i < labels.size; i++ )
	{
		rows[ i ] = self hud_row( i, labels[ i ] );
	}
	surf = self hud_row( labels.size, "ground surface" );
	surf SetText( "" );
	shown = "";
	while ( true )
	{
		wait 0.25;
		a = ( level.gwp_hud_on ? 1 : 0 );
		foreach ( e in rows )
		{
			e.alpha = a;
		}
		foreach ( e in self.gwp_labels )
		{
			e.alpha = a;
		}
		surf.alpha = a;
		if ( !level.gwp_hud_on )
		{
			continue;
		}
		p = self.origin;
		yaw = self GetPlayerAngles()[ 1 ];
		f = AnglesToForward( ( 0, yaw, 0 ) );
		r = AnglesToRight( ( 0, yaw, 0 ) );
		tr = ptrace( p + ( 0, 0, 24 ), p - ( 0, 0, 48 ), 0, 0, self );
		bt = BulletTrace( p + ( 0, 0, 24 ), p - ( 0, 0, 48 ), false, self );
		rows[ 0 ] SetValue( stance_code( self GetStance() ) );
		rows[ 1 ] SetValue( self IsOnGround() );
		rows[ 2 ] SetValue( Int( vz0( tr[ "normal" ] ) * 1000 ) );
		rows[ 3 ] SetValue( Floor( ( tr[ "position" ][ 2 ] - bt[ "position" ][ 2 ] ) * 10 ) / 10 );
		rows[ 4 ] SetValue( Int( clear_dist( p, f, 14, 2, 2, 64, self ) ) );
		rows[ 5 ] SetValue( Int( clear_dist( p, f * -1, 14, 2, 2, 64, self ) ) );
		rows[ 6 ] SetValue( Int( clear_dist( p, r * -1, 14, 2, 2, 64, self ) ) );
		rows[ 7 ] SetValue( Int( clear_dist( p, r, 14, 2, 2, 64, self ) ) );
		gf = ptrace( p + f * 32 + ( 0, 0, 24 ), p + f * 32 - ( 0, 0, 48 ), 0, 0, self );
		gb = ptrace( p - f * 32 + ( 0, 0, 24 ), p - f * 32 - ( 0, 0, 48 ), 0, 0, self );
		rows[ 8 ] SetValue( ( gf[ "fraction" ] < 1 ? Floor( ( gf[ "position" ][ 2 ] - p[ 2 ] ) * 10 ) / 10 : 999 ) );
		rows[ 9 ] SetValue( ( gb[ "fraction" ] < 1 ? Floor( ( gb[ "position" ][ 2 ] - p[ 2 ] ) * 10 ) / 10 : 999 ) );
)GSC",
        R"GSC(		rows[ 10 ] SetValue( Int( StrTok( self touching_word(), "/" )[ 0 ] ) );
		rows[ 11 ] SetValue( level.gwp_n );
		rows[ 12 ] SetValue( level.gwp_ok );
		s = word( tr[ "surfacetype" ] );
		if ( s != shown )
		{
			surf SetText( s );
			shown = s;
		}
	}
}

//*****************************************************************************
// Console: set gwp_cmd <command>
//*****************************************************************************

function command_loop()
{
	while ( true )
	{
		wait 0.25;
		cmd = GetDvarString( "gwp_cmd", "" );
		if ( cmd == "" )
		{
			continue;
		}
		SetDvar( "gwp_cmd", "" );
		args = StrTok( cmd, " " );
		players = GetPlayers();
		if ( players.size == 0 || args.size == 0 )
		{
			continue;
		}
		player = players[ 0 ];
		gw_log( "cmd", "cmd=" + word( cmd ) );
		switch ( args[ 0 ] )
		{
			case "probe":
				player thread manual_probe();
				break;
			case "force":
				player thread manual_force();
				break;
			case "auto":
				if ( !level.gwp_busy )
				{
					from = ( args.size > 1 ? Int( args[ 1 ] ) : 0 );
					to = ( args.size > 2 ? Int( args[ 2 ] ) : 0 );
					player thread run_points( from, to );
				}
				break;
			case "grid":
				if ( !level.gwp_busy )
				{
					player thread run_grid();
				}
				break;
			case "stop":
				level notify( "gwp_stop" );
				break;
			case "summary":
				summary();
				break;
			case "hud":
				level.gwp_hud_on = !level.gwp_hud_on;
				break;
			case "allow":
				level.gwp_allow = !level.gwp_allow;
				gw_say( "GWP AllowProne(true) every frame: " + level.gwp_allow );
				break;
			case "env":
				player env();
				break;
			default:
				gw_say( "GWP unknown command " + cmd );
				break;
		}
	}
}

)GSC",
    };
    std::string Out;
    for (const char* P : Pieces) Out += P;
    // the line ends the Mod Tools' own scripts use
    std::string Crlf;
    for (char C : Out) Crlf += C == '\n' ? std::string("\r\n") : std::string(1, C);
    return Crlf;
}
}
