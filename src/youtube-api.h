#pragma once

// YouTube Live integration: lets a target create its own YouTube broadcast
// (title, description, privacy, thumbnail) and bind it to the target's
// stream key right before the output starts.
//
// Auth uses Google's OAuth "TV and limited input devices" flow with a client
// ID/secret the user creates in their own Google Cloud project. OBS's built-in
// YouTube client can't be reused by plugins.
//
// All functions here are blocking network calls; never call them on the UI
// thread.

#include <string>
#include <functional>
#include <optional>

struct YouTubeTargetConfig {
    bool enabled = false;
    std::string title;
    std::string description;
    std::string privacy = "public"; // public / unlisted / private
    std::string thumbnailPath;
    bool madeForKids = false;
};

struct YouTubeAuthConfig {
    std::string clientId;
    std::string clientSecret;
    std::string refreshToken;
    std::string channelTitle; // display only
};

namespace youtube {

bool IsSupported(); // false on non-Windows builds

// Stored in the plugin's module config folder, shared by all profiles.
YouTubeAuthConfig LoadAuth();
void SaveAuth(const YouTubeAuthConfig& auth);

struct DeviceCode {
    std::string deviceCode;
    std::string userCode;
    std::string verificationUrl;
    int interval = 5;
    int expiresIn = 1800;
};

// Step 1 of sign-in. Returns error text on failure.
std::optional<DeviceCode> RequestDeviceCode(const std::string& clientId, std::string& error);

enum class PollResult { Pending, SlowDown, Granted, Failed };

// Step 2: poll until Granted/Failed. On Granted, refreshToken and channelTitle are filled.
PollResult PollDeviceToken(const std::string& clientId, const std::string& clientSecret,
    const std::string& deviceCode, std::string& refreshToken, std::string& channelTitle,
    std::string& error);

struct BroadcastResult {
    bool ok = false;
    std::string error;          // fatal error (stream should not start)
    std::string warning;        // non-fatal (e.g. thumbnail rejected)
    std::string broadcastId;
};

// Creates a broadcast, binds it to the YouTube stream whose key == streamKey,
// and uploads the thumbnail. Uses the stored auth.
BroadcastResult PrepareBroadcast(const YouTubeTargetConfig& cfg, const std::string& streamKey);

} // namespace youtube
