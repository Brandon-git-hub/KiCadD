/*
 * KiCadD MCP - launch_kicad / close_kicad (roadmap phase 4, item 3).
 *
 * launch_kicad starts KiCad on the socket this kicadd-mcp already dials, so every other tool
 * works on it right away: the headless server (kicad-cli api-server) or the desktop PCB editor
 * (pcbnew, which serves on KICADD_API_SOCKET). Only KiCadD's own programs, from the folder
 * kicadd-mcp is in (or its build tree), never a command given by the caller. Refused while
 * something answers on the socket already.
 *
 * close_kicad stops the KiCad started here (nothing else: a server or editor started some
 * other way is the user's), refused while the board or the schematic has unsaved changes
 * unless discard.
 *
 * In HTTP mode both refuse unless kicadd-mcp was started with --allow-launch.
 *
 * This file is part of KiCadD and is licensed GPL-3.0-or-later, like KiCad.
 */

#include "tools.h"
#include "ipc_client.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>

#include <kicadd/board.pb.h>
#include <kicadd/schematic.pb.h>

#if defined( _WIN32 )
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#if defined( __APPLE__ )
#include <mach-o/dyld.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace kb = kicadd::board;
namespace ks = kicadd::schematic;

namespace kicadd::mcp
{

namespace
{

TOOL_RESULT fail( const std::string& aMsg ) { return { aMsg, true }; }


fs::path toPath( const std::string& aUtf8 )
{
#ifdef _WIN32
    return fs::path( std::u8string( aUtf8.begin(), aUtf8.end() ) );
#else
    return fs::path( aUtf8 );
#endif
}


std::string fromPath( const fs::path& aPath )
{
    std::u8string s = aPath.u8string();
    return std::string( s.begin(), s.end() );
}


#if defined( _WIN32 )
const char* EXE = ".exe";
#else
const char* EXE = "";
#endif


fs::path selfDir()
{
#if defined( _WIN32 )
    std::wstring buf( 32768, L'\0' );
    DWORD        n = GetModuleFileNameW( nullptr, buf.data(), (DWORD) buf.size() );
    buf.resize( n );
    return fs::path( buf ).parent_path();
#elif defined( __APPLE__ )
    char     buf[4096];
    uint32_t size = sizeof buf;

    if( _NSGetExecutablePath( buf, &size ) == 0 )
        return fs::weakly_canonical( fs::path( buf ) ).parent_path();

    return {};
#else
    std::error_code ec;
    return fs::read_symlink( "/proc/self/exe", ec ).parent_path();
#endif
}


/// KiCadD's programs next to kicadd-mcp: a build tree (BUILD/kicad/kicad-cli, BUILD/pcbnew/pcbnew,
/// run with KICAD_RUN_FROM_BUILD_DIR) or an installed / portable folder (bin/).
struct PROGRAMS
{
    fs::path cli, pcbnew;
    bool     buildTree = false;
};


PROGRAMS findPrograms()
{
    PROGRAMS        p;
    fs::path        dir = selfDir();
    std::error_code ec;

    if( fs::is_regular_file( dir / "kicad" / ( std::string( "kicad-cli" ) + EXE ), ec ) )
    {
        p.cli = dir / "kicad" / ( std::string( "kicad-cli" ) + EXE );
        p.pcbnew = dir / "pcbnew" / ( std::string( "pcbnew" ) + EXE );
        p.buildTree = true;
    }
    else
    {
        p.cli = dir / ( std::string( "kicad-cli" ) + EXE );
        p.pcbnew = dir / ( std::string( "pcbnew" ) + EXE );
    }

    return p;
}


/// the socket's file path (ipc://PATH); empty for tcp://
std::string socketPath( const IPC_CLIENT& aIpc )
{
    const std::string& url = aIpc.Url();
    return url.rfind( "ipc://", 0 ) == 0 ? url.substr( 6 ) : std::string();
}


bool transportError( const std::string& aErr )
{
    return aErr.starts_with( "send to KiCad failed" ) || aErr.starts_with( "receive from KiCad failed" )
           || aErr.starts_with( "IPC client not connected" );
}


/// Is a KiCad answering on the socket? aFile: its board (empty: none open)
bool answering( IPC_CLIENT& aIpc, std::string& aFile )
{
#if !defined( _WIN32 )
    // no socket file: nobody listens (and no 5 s send timeout to wait out)
    std::error_code ec;

    if( std::string s = socketPath( aIpc ); !s.empty() && !fs::exists( toPath( s ), ec ) )
        return false;
#endif

    kb::BoardSummaryResponse r;
    std::string              err;

    if( aIpc.Call( kb::GetBoardSummary(), r, err ) )
    {
        aFile = r.file();
        return true;
    }

    aFile.clear();
    return !transportError( err );
}


std::string tail( const fs::path& aLog, size_t aBytes = 1200 )
{
    std::ifstream in( aLog, std::ios::binary );
    std::string   s( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );

    if( s.size() > aBytes )
        s = "..." + s.substr( s.size() - aBytes );

    while( !s.empty() && ( s.back() == '\n' || s.back() == '\r' ) )
        s.pop_back();

    return s;
}


struct CHILD
{
#if defined( _WIN32 )
    HANDLE process = nullptr;
    DWORD  pid = 0;
#else
    pid_t  pid = 0;
#endif
    std::string mode;     ///< "server" / "desktop"
    std::string project;
    fs::path    log;

    bool running() const
    {
#if defined( _WIN32 )
        return process && WaitForSingleObject( process, 0 ) == WAIT_TIMEOUT;
#else
        return pid > 0 && kill( pid, 0 ) == 0;
#endif
    }

    /// reap it when it has exited; the exit code, or -1 while it runs
    int exited()
    {
#if defined( _WIN32 )
        DWORD code = 0;

        if( !process || WaitForSingleObject( process, 0 ) == WAIT_TIMEOUT )
            return -1;

        GetExitCodeProcess( process, &code );
        return (int) code;
#else
        int status = 0;

        if( pid <= 0 )
            return 0;

        if( waitpid( pid, &status, WNOHANG ) == 0 )
            return -1;

        pid = 0;
        return WIFEXITED( status ) ? WEXITSTATUS( status ) : 128 + WTERMSIG( status );
#endif
    }

    unsigned long id() const { return (unsigned long) pid; }

    void forget()
    {
#if defined( _WIN32 )
        if( process )
            CloseHandle( process );

        process = nullptr;
#endif
        pid = 0;
        mode.clear();
    }
};


struct STATE
{
    bool  allowed = false;
    CHILD child;
};


#if defined( _WIN32 )
std::wstring widen( const std::string& aUtf8 )
{
    if( aUtf8.empty() )
        return {};

    int          n = MultiByteToWideChar( CP_UTF8, 0, aUtf8.data(), (int) aUtf8.size(), nullptr, 0 );
    std::wstring w( n, L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, aUtf8.data(), (int) aUtf8.size(), w.data(), n );
    return w;
}


std::wstring quoteArg( const std::wstring& aArg )
{
    // CommandLineToArgvW rules
    std::wstring out = L"\"";
    size_t       slashes = 0;

    for( wchar_t c : aArg )
    {
        if( c == L'\\' )
        {
            slashes++;
            continue;
        }

        if( c == L'"' )
            out.append( slashes * 2 + 1, L'\\' );
        else
            out.append( slashes, L'\\' );

        slashes = 0;
        out += c;
    }

    out.append( slashes * 2, L'\\' );
    return out + L"\"";
}
#endif


/// Start aArgv[0] detached from this process (own session / no console), output to aLog,
/// with aSet added to and aUnset taken out of the environment.
bool spawn( CHILD& aChild, const std::vector<std::string>& aArgv, const std::vector<std::pair<std::string, std::string>>& aSet,
            const std::vector<std::string>& aUnset, const fs::path& aLog, bool aGui, std::string& aErr )
{
#if defined( _WIN32 )
    // the child inherits this process's environment: change it for the call, then put it back
    std::vector<std::pair<std::wstring, std::optional<std::wstring>>> saved;

    auto save = [&]( const std::string& k )
    {
        std::wstring key = widen( k );
        DWORD        n = GetEnvironmentVariableW( key.c_str(), nullptr, 0 );
        std::optional<std::wstring> v;

        if( n > 0 )
        {
            std::wstring buf( n, L'\0' );
            buf.resize( GetEnvironmentVariableW( key.c_str(), buf.data(), n ) );
            v = buf;
        }

        saved.push_back( { key, v } );
    };

    for( const std::string& k : aUnset )
    {
        save( k );
        SetEnvironmentVariableW( widen( k ).c_str(), nullptr );
    }

    for( const auto& kv : aSet )
        save( kv.first );

    for( const auto& [k, v] : aSet )
        SetEnvironmentVariableW( widen( k ).c_str(), widen( v ).c_str() );

    std::wstring cmd;

    for( const std::string& a : aArgv )
        cmd += ( cmd.empty() ? L"" : L" " ) + quoteArg( widen( a ) );

    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE log = CreateFileW( aLog.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof si;

    if( log != INVALID_HANDLE_VALUE )
    {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = log;
        si.hStdError = log;
    }

    BOOL ok = CreateProcessW( aArgv[0].empty() ? nullptr : widen( aArgv[0] ).c_str(), cmd.data(), nullptr, nullptr,
                              log != INVALID_HANDLE_VALUE, ( aGui ? 0 : CREATE_NO_WINDOW ) | CREATE_NEW_PROCESS_GROUP,
                              nullptr, nullptr, &si, &pi );
    DWORD lastError = GetLastError();

    // the environment of this process back as it was
    for( auto it = saved.rbegin(); it != saved.rend(); ++it )
        SetEnvironmentVariableW( it->first.c_str(), it->second ? it->second->c_str() : nullptr );

    if( log != INVALID_HANDLE_VALUE )
        CloseHandle( log );

    if( !ok )
    {
        aErr = "CreateProcess failed (error " + std::to_string( lastError ) + ")";
        return false;
    }

    CloseHandle( pi.hThread );
    aChild.process = pi.hProcess;
    aChild.pid = pi.dwProcessId;
    return true;
#else
    (void) aGui;
    std::vector<std::string> env;

    for( char** e = environ; e && *e; ++e )
    {
        std::string kv = *e;
        std::string k = kv.substr( 0, kv.find( '=' ) );
        bool        drop = std::find( aUnset.begin(), aUnset.end(), k ) != aUnset.end();

        for( const auto& s : aSet )
            drop = drop || s.first == k;

        if( !drop )
            env.push_back( kv );
    }

    for( const auto& [k, v] : aSet )
        env.push_back( k + "=" + v );

    std::vector<char*> argv, envp;

    for( const std::string& a : aArgv )
        argv.push_back( const_cast<char*>( a.c_str() ) );

    for( const std::string& e : env )
        envp.push_back( const_cast<char*>( e.c_str() ) );

    argv.push_back( nullptr );
    envp.push_back( nullptr );

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    posix_spawn_file_actions_init( &fa );
    posix_spawnattr_init( &attr );
    posix_spawn_file_actions_addopen( &fa, 0, "/dev/null", O_RDONLY, 0 );
    posix_spawn_file_actions_addopen( &fa, 1, aLog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644 );
    posix_spawn_file_actions_adddup2( &fa, 1, 2 );

    // its own session: a Ctrl-C or hangup meant for kicadd-mcp's client is not passed on
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETSID );
#endif

    pid_t pid = 0;
    int   rv = posix_spawn( &pid, argv[0], &fa, &attr, argv.data(), envp.data() );
    posix_spawn_file_actions_destroy( &fa );
    posix_spawnattr_destroy( &attr );

    if( rv != 0 )
    {
        aErr = std::string( "posix_spawn failed: " ) + std::strerror( rv );
        return false;
    }

    aChild.pid = pid;
    return true;
#endif
}


void terminate( CHILD& aChild, int aGraceMs )
{
#if defined( _WIN32 )
    (void) aGraceMs;
    TerminateProcess( aChild.process, 1 );
    WaitForSingleObject( aChild.process, 10000 );
#else
    // the server stops on SIGTERM (it saves nothing); the editor is ended the same way: the
    // decision about its unsaved changes was taken before (discard)
    kill( aChild.pid, SIGTERM );

    for( int waited = 0; waited < aGraceMs; waited += 100 )
    {
        if( aChild.exited() >= 0 )
            return;

        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }

    kill( aChild.pid, SIGKILL );
    waitpid( aChild.pid, nullptr, 0 );
#endif
}


std::string notAllowed()
{
    return "not allowed: this kicadd-mcp serves HTTP and was started without --allow-launch (starting and stopping "
           "programs on the server is off by default there); start it with --allow-launch, or run KiCad yourself";
}


std::string what( IPC_CLIENT& aIpc )
{
    std::string file;

    if( !answering( aIpc, file ) )
        return {};

    return file.empty() ? std::string( "no board open" ) : fromPath( toPath( file ).filename() );
}


TOOL_RESULT launch( IPC_CLIENT& aIpc, STATE& aState, const json& a )
{
    if( !aState.allowed )
        return fail( notAllowed() );

    const std::string mode = a.value( "mode", std::string( "server" ) );

    if( mode != "server" && mode != "desktop" )
        return fail( "mode: \"server\" (headless kicad-cli api-server) or \"desktop\" (the PCB editor)" );

    const std::string sock = socketPath( aIpc );

    if( sock.empty() )
        return fail( "kicadd-mcp dials " + aIpc.Url() + ": KiCad serves only ipc:// sockets, so nothing can be started for it" );

    // the KiCad started here is still up
    if( aState.child.exited() < 0 && !aState.child.mode.empty() )
    {
        return fail( "already running: the " + aState.child.mode + " started here (pid " + std::to_string( aState.child.id() )
                     + ", " + ( what( aIpc ).empty() ? "not answering" : what( aIpc ) )
                     + "); open_board switches the server's board, close_kicad stops it" );
    }

    aState.child.forget();

    if( std::string open = what( aIpc ); !open.empty() )
    {
        return fail( "a KiCad already answers on " + sock + " (" + open
                     + ") and every tool works on it; it was not started here, so close it where it was started "
                       "(open_board switches a server's board)" );
    }

    // the project: a folder (its one .kicad_pro), a .kicad_pro, .kicad_pcb or .kicad_sch
    fs::path        project, board;
    std::error_code ec;

    if( a.contains( "path" ) && a["path"].is_string() && !a["path"].get<std::string>().empty() )
    {
        project = toPath( a["path"].get<std::string>() );

        if( !project.is_absolute() )
            return fail( "path must be absolute (on the server): " + fromPath( project ) );

        if( fs::is_directory( project, ec ) )
        {
            std::vector<fs::path> pros;

            for( const auto& e : fs::directory_iterator( project, ec ) )
            {
                if( e.path().extension() == ".kicad_pro" )
                    pros.push_back( e.path() );
            }

            if( pros.size() != 1 )
                return fail( fromPath( project ) + " has " + std::to_string( pros.size() ) + " .kicad_pro files: give the one to open" );

            project = pros[0];
        }

        const fs::path ext = project.extension();

        if( ext != ".kicad_pro" && ext != ".kicad_pcb" && ext != ".kicad_sch" )
            return fail( "path: a project folder, .kicad_pro, .kicad_pcb or .kicad_sch" );

        if( !fs::is_regular_file( project, ec ) )
            return fail( "no such file: " + fromPath( project ) + ( ext == ".kicad_pro" ? " (new_project makes one)" : "" ) );

        board = fs::path( project ).replace_extension( ".kicad_pcb" );

        if( !fs::is_regular_file( board, ec ) )
            board.clear();
    }

    if( mode == "desktop" && board.empty() )
        return fail( "desktop: the PCB editor needs a board (path to a project with a .kicad_pcb); the agent works on the board open there" );

#if !defined( _WIN32 ) && !defined( __APPLE__ )
    if( mode == "desktop" && !std::getenv( "DISPLAY" ) && !std::getenv( "WAYLAND_DISPLAY" ) )
        return fail( "desktop: no display here (DISPLAY / WAYLAND_DISPLAY unset); mode=\"server\" runs without one" );
#endif

    PROGRAMS progs = findPrograms();
    fs::path exe = mode == "server" ? progs.cli : progs.pcbnew;

    if( !fs::is_regular_file( exe, ec ) )
        return fail( "not found: " + fromPath( exe ) + " (launch_kicad starts only KiCadD's own programs, next to kicadd-mcp)" );

    std::vector<std::string> argv = { fromPath( exe ) };

    if( mode == "server" )
    {
        argv.insert( argv.end(), { "api-server", "--socket", sock } );

        if( !project.empty() )
            argv.push_back( fromPath( project ) );
    }
    else
    {
        argv.push_back( fromPath( board ) );
    }

    fs::path sockFile = toPath( sock );
    fs::create_directories( sockFile.parent_path(), ec );

#if !defined( _WIN32 )
    // nobody answers there (checked above): a leftover file from a killed KiCad would send the
    // new one to api-<pid>.sock when another KiCad holds the folder's api.lock
    if( fs::exists( sockFile, ec ) )
        fs::remove( sockFile, ec );
#endif

    std::vector<std::pair<std::string, std::string>> set;
    std::vector<std::string>                         unset;

    if( progs.buildTree )
        set.push_back( { "KICAD_RUN_FROM_BUILD_DIR", "1" } );
    else
        unset.push_back( "KICAD_RUN_FROM_BUILD_DIR" );

    if( mode == "desktop" )
    {
        set.push_back( { "KICADD_API_SOCKET", sock } );
        unset.push_back( "KICADD_NO_API" );
    }

    fs::path    log = sockFile.parent_path() / ( "kicadd-" + mode + ".log" );
    std::string err;
    CHILD       child;

    if( !spawn( child, argv, set, unset, log, mode == "desktop", err ) )
        return fail( "could not start " + fromPath( exe ) + ": " + err );

    child.mode = mode;
    child.project = fromPath( project );
    child.log = log;

    // until it answers (with the board loaded)
    const double waitS = std::clamp( a.value( "wait_s", mode == "desktop" ? 120.0 : 60.0 ), 5.0, 600.0 );
    const auto   t0 = std::chrono::steady_clock::now();
    const auto   deadline = t0 + std::chrono::milliseconds( (int) ( waitS * 1000 ) );
    std::string  file;
    bool         up = false;

    while( std::chrono::steady_clock::now() < deadline )
    {
        if( int code = child.exited(); code >= 0 )
        {
            child.forget();
            std::string t = tail( log );
            return fail( "the " + mode + " exited (code " + std::to_string( code ) + ") before answering; log " + fromPath( log )
                         + ( t.empty() ? std::string() : ":\n" + t ) );
        }

        if( answering( aIpc, file ) && ( board.empty() || !file.empty() ) )
        {
            up = true;
            break;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
    }

    const double took = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();
    char         secs[32];
    std::snprintf( secs, sizeof secs, "%.1f", took );
    aState.child = child;

    std::string out = "started the " + mode + " (pid " + std::to_string( child.id() ) + ") on " + sock;

    if( !up )
    {
        return fail( out + ", but it has not answered in " + secs + " s (still running; log " + fromPath( log )
                     + "): try board_summary later, close_kicad stops it" );
    }

    out += " in " + std::string( secs ) + " s; log " + fromPath( log ) + "\n";

    // what is open: the board's summary, and whether there is a schematic
    kb::BoardSummaryResponse r;

    if( aIpc.Call( kb::GetBoardSummary(), r, err ) && !r.file().empty() )
    {
        out += "board " + fromPath( toPath( r.file() ).filename() ) + ": " + std::to_string( r.footprints() ) + " footprints, "
               + std::to_string( r.nets() ) + " nets, " + std::to_string( r.copper_layers() ) + " copper layers\n";
    }
    else if( mode == "server" )
    {
        out += "no board open (open_board / new_project)\n";
    }

    ks::SchSummary sch;

    if( mode == "server" && aIpc.Call( ks::GetSchSummary(), sch, err ) && !sch.file().empty() )
        out += "schematic " + fromPath( toPath( sch.file() ).filename() ) + ": " + std::to_string( sch.symbols_size() ) + " symbols\n";

    return { out };
}


TOOL_RESULT close( IPC_CLIENT& aIpc, STATE& aState, const json& a )
{
    if( !aState.allowed )
        return fail( notAllowed() );

    const bool  discard = a.value( "discard", false );
    CHILD&      child = aState.child;
    std::string open = what( aIpc );

    if( child.mode.empty() || child.exited() >= 0 )
    {
        child.forget();

        if( open.empty() )
            return fail( "nothing to close: no KiCad answers on " + aIpc.Url() );

        return fail( "the KiCad on " + aIpc.Url() + " (" + open
                     + ") was not started by launch_kicad here: close it where it was started (the desktop "
                       "editor: the user closes it)" );
    }

    // unsaved work: board and schematic
    std::string              err, unsaved;
    kb::HistoryResponse      history;
    ks::SchSummary           sch;
    bool                     boardDirty = false, schDirty = false;

    if( !open.empty() )
    {
        boardDirty = aIpc.Call( kb::GetHistory(), history, err ) && history.modified();
        schDirty = aIpc.Call( ks::GetSchSummary(), sch, err ) && sch.modified();
    }

    if( ( boardDirty || schDirty ) && !discard )
    {
        if( boardDirty )
            unsaved += "\nthe board (" + open + ") has unsaved changes: save_board" + UnsavedBoardChanges( aIpc );

        if( schDirty )
            unsaved += "\nthe schematic (" + fromPath( toPath( sch.file() ).filename() ) + ") has unsaved changes: sch_save";

        return fail( "not closed: save first, or discard=true to drop the changes" + unsaved );
    }

    const std::string mode = child.mode;
    const unsigned long pid = child.id();
    terminate( child, mode == "desktop" ? 5000 : 15000 );

#if !defined( _WIN32 )
    // a killed KiCad leaves its socket file behind
    std::error_code ec;

    if( std::string s = socketPath( aIpc ); !s.empty() && fs::exists( toPath( s ), ec ) )
        fs::remove( toPath( s ), ec );
#endif

    child.forget();

    std::string out = "closed the " + mode + " (pid " + std::to_string( pid ) + ")";

    if( boardDirty || schDirty )
        out += std::string( "; unsaved changes dropped (" ) + ( boardDirty ? "board" : "" )
               + ( boardDirty && schDirty ? ", " : "" ) + ( schDirty ? "schematic" : "" ) + ")";

    return { out + "; launch_kicad starts one again\n" };
}

} // namespace


void AddLaunchTools( std::vector<TOOL>& aTools, IPC_CLIENT& aIpc, bool aAllowed )
{
    auto state = std::make_shared<STATE>();
    state->allowed = aAllowed;

    json launchSchema = { { "type", "object" },
                          { "properties",
                            { { "mode", { { "type", "string" }, { "enum", { "server", "desktop" } } } },
                              { "path", { { "type", "string" }, { "description", "absolute: folder, .kicad_pro, .kicad_pcb or .kicad_sch" } } },
                              { "wait_s", { { "type", "number" } } } } } };

    aTools.push_back( { "launch_kicad",
                        "Start KiCad for the agent on the socket kicadd-mcp uses, so every tool works on it at once: "
                        "mode \"server\" (default: headless kicad-cli api-server; path optional, new_project / open_board "
                        "after) or \"desktop\" (the PCB editor with path's board, for a user to watch and route; needs a "
                        "display). path: a project folder (its one .kicad_pro), .kicad_pro, .kicad_pcb or .kicad_sch, "
                        "absolute on the server. Waits until it answers (wait_s, default 60 / 120 for the desktop) and "
                        "returns what is open and the log file. Refused while a KiCad already answers there. Only "
                        "KiCadD's own programs next to kicadd-mcp. HTTP mode: only with --allow-launch.",
                        launchSchema, [&aIpc, state]( const json& a ) { return launch( aIpc, *state, a ); } } );

    json closeSchema = { { "type", "object" }, { "properties", { { "discard", { { "type", "boolean" } } } } } };

    aTools.push_back( { "close_kicad",
                        "Stop the KiCad that launch_kicad started (server or desktop editor); one started any other way "
                        "is left alone (refused). Refused while the board or the schematic has unsaved changes (listed, "
                        "with board_diff) unless discard=true. HTTP mode: only with --allow-launch.",
                        closeSchema, [&aIpc, state]( const json& a ) { return close( aIpc, *state, a ); } } );
}

} // namespace kicadd::mcp
