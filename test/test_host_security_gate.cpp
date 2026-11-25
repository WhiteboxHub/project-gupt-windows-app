#include <iostream>
#include <cassert>
#include <atomic>
#include <string>
#include <vector>
#include "../Shared/Protocol.h"

using namespace gupt::shared;

// Mirrors the host authorization state machine and ExtractJsonField logic in Launcher/main.cpp
static std::string ExtractJsonField(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\":\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    size_t start = pos + pattern.size();
    size_t end = json.find("\"", start);
    if (end == std::string::npos) return "";
    return json.substr(start, end - start);
}

struct MockHostGate {
    std::atomic<bool> sessionActive{false};
    std::atomic<bool> hostApproved{false};
    std::atomic<bool> approvalPending{false};
    int mouseInjectedCount = 0;
    int keyInjectedCount = 0;
    int clipboardCount = 0;
    int framesCapturedCount = 0;
    std::vector<bool> sentResponses;

    void onConnectRequest() {
        if (sessionActive.load() && hostApproved.load()) {
            sentResponses.push_back(false);
            return;
        }
        approvalPending = true;
        // Does NOT set sessionActive or hostApproved yet!
    }

    void resolveApproval(bool accept) {
        approvalPending = false;
        if (accept) {
            hostApproved = true;
            sessionActive = true;
            sentResponses.push_back(true);
        } else {
            hostApproved = false;
            sessionActive = false;
            sentResponses.push_back(false);
        }
    }

    void onDisconnect() {
        sessionActive = false;
        hostApproved = false;
        approvalPending = false;
    }

    void dispatch(MessageType t) {
        if (!sessionActive.load() || !hostApproved.load()) {
            return; // Strict authorization gate
        }
        if (t == MessageType::MouseEvent) mouseInjectedCount++;
        else if (t == MessageType::KeyboardEvent) keyInjectedCount++;
        else if (t == MessageType::ClipboardText) clipboardCount++;
    }

    void captureTick() {
        if (sessionActive.load() && hostApproved.load()) {
            framesCapturedCount++;
        }
    }
};

int main() {
    std::cout << "[TEST] Running Windows Host Security & Approval Gate unit tests..." << std::endl;

    // 1. JSON credential extraction
    std::string regJson = "{\"sessionId\":\"a1b2c3d4\",\"hostSecret\":\"sec123\",\"pairingPin\":\"654321\",\"expiresInMs\":900000}";
    assert(ExtractJsonField(regJson, "sessionId") == "a1b2c3d4");
    assert(ExtractJsonField(regJson, "hostSecret") == "sec123");
    assert(ExtractJsonField(regJson, "pairingPin") == "654321");
    assert(ExtractJsonField(regJson, "missing") == "");

    // 2. Reject all input & capture before ConnectRequest
    MockHostGate gate;
    gate.dispatch(MessageType::MouseEvent);
    gate.dispatch(MessageType::KeyboardEvent);
    gate.dispatch(MessageType::ClipboardText);
    gate.captureTick();
    assert(gate.mouseInjectedCount == 0);
    assert(gate.keyInjectedCount == 0);
    assert(gate.clipboardCount == 0);
    assert(gate.framesCapturedCount == 0);

    // 3. ConnectRequest queues approval without activating session
    gate.onConnectRequest();
    assert(gate.approvalPending.load() == true);
    assert(gate.sessionActive.load() == false);
    assert(gate.hostApproved.load() == false);
    gate.dispatch(MessageType::MouseEvent);
    gate.captureTick();
    assert(gate.mouseInjectedCount == 0);
    assert(gate.framesCapturedCount == 0);

    // 4. Rejecting prevents activation and keeps input blocked
    gate.resolveApproval(false);
    assert(gate.sentResponses.size() == 1 && gate.sentResponses[0] == false);
    assert(gate.sessionActive.load() == false);
    assert(gate.hostApproved.load() == false);
    gate.dispatch(MessageType::KeyboardEvent);
    assert(gate.keyInjectedCount == 0);

    // 5. Accepting activates session and permits input + capture
    gate.onConnectRequest();
    gate.resolveApproval(true);
    assert(gate.sentResponses.size() == 2 && gate.sentResponses[1] == true);
    assert(gate.sessionActive.load() == true);
    assert(gate.hostApproved.load() == true);
    gate.dispatch(MessageType::MouseEvent);
    gate.dispatch(MessageType::KeyboardEvent);
    gate.dispatch(MessageType::ClipboardText);
    gate.captureTick();
    assert(gate.mouseInjectedCount == 1);
    assert(gate.keyInjectedCount == 1);
    assert(gate.clipboardCount == 1);
    assert(gate.framesCapturedCount == 1);

    // 6. Disconnect resets approval and blocks subsequent input + capture
    gate.onDisconnect();
    assert(gate.sessionActive.load() == false);
    assert(gate.hostApproved.load() == false);
    gate.dispatch(MessageType::MouseEvent);
    gate.dispatch(MessageType::KeyboardEvent);
    gate.dispatch(MessageType::ClipboardText);
    gate.captureTick();
    assert(gate.mouseInjectedCount == 1);
    assert(gate.keyInjectedCount == 1);
    assert(gate.clipboardCount == 1);
    assert(gate.framesCapturedCount == 1);

    std::cout << "[SUCCESS] All Windows Host Security Gate tests passed!" << std::endl;
    return 0;
}

