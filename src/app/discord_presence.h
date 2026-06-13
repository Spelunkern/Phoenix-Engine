#pragma once

namespace phoenix::app
{
    // Best-effort Discord Rich Presence. Spawns a detached background worker
    // that connects to the local Discord IPC socket and publishes the app's
    // presence ("Playing Phoenix Engine" + your art asset icon). Completely
    // silent and harmless if Discord isn't running or the client id is unset —
    // it never blocks the caller and the process-exit path tears it down.
    //
    // Configure your Discord Application ID and art-asset key in
    // discord_presence.cpp (kDiscordClientId / kDiscordLargeImage).
    void start_discord_presence();
}
