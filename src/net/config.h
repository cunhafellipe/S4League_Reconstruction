// Minimal version.ini reader for the auth server endpoint ([SERVER] IP1/PORT1).
#pragma once
#include <string>

namespace cfg {
// Reads version.ini (from the exe working dir). Fills ip/port from [SERVER]
// IP1/PORT1. Returns true if an IP was found.
bool ServerEndpoint(std::string& ip, int& port);
}
