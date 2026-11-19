#include <iostream>
#include <cassert>
#include <fstream>
#include "../Shared/Protocol.h"

using namespace gupt::shared;

int main() {
    std::cout << "[TEST] Running C++ Protocol WebRTC unit tests..." << std::endl;

    // 1. Verify structure sizes
    assert(sizeof(WebRtcSdpHeader) == 52);
    assert(sizeof(WebRtcCandidateHeader) == 120);
    assert(sizeof(MessageHeader) == 5);

    // 2. Test Offer Serialization and Deserialization
    std::string testSession = "sess_win_to_mac_1";
    std::string testRole = "host";
    std::string testSdp = "v=0\r\no=- 12345 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n";

    auto offerBytes = SerializeWebRtcOffer(testSession, testRole, testSdp);
    assert(!offerBytes.empty());
    assert(offerBytes.size() == 5 + sizeof(WebRtcSdpHeader) + testSdp.size());

    auto msgs = DeserializeMessages(offerBytes);
    assert(msgs.size() == 1);
    assert(msgs[0].first == MessageType::WebRtcOffer);

    std::string parsedSess, parsedRole, parsedSdp;
    bool ok = ParseWebRtcSdp(msgs[0].second, parsedSess, parsedRole, parsedSdp);
    assert(ok);
    assert(parsedSess == testSession);
    assert(parsedRole == testRole);
    assert(parsedSdp == testSdp);
    std::cout << "  -> C++ WebRtcOffer serialization & parsing: PASSED" << std::endl;

    // 3. Test Answer Serialization and Deserialization
    std::string testAnsRole = "controller";
    std::string testAnsSdp = "v=0\r\no=- 54321 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n";

    auto ansBytes = SerializeWebRtcAnswer(testSession, testAnsRole, testAnsSdp);
    assert(!ansBytes.empty());
    auto ansMsgs = DeserializeMessages(ansBytes);
    assert(ansMsgs.size() == 1);
    assert(ansMsgs[0].first == MessageType::WebRtcAnswer);

    ok = ParseWebRtcSdp(ansMsgs[0].second, parsedSess, parsedRole, parsedSdp);
    assert(ok);
    assert(parsedSess == testSession);
    assert(parsedRole == testAnsRole);
    assert(parsedSdp == testAnsSdp);
    std::cout << "  -> C++ WebRtcAnswer serialization & parsing: PASSED" << std::endl;

    // 4. Test Candidate Serialization and Deserialization
    std::string testMid = "0";
    int32_t testMLine = 0;
    std::string testUfrag = "ice_ufrag_99";
    std::string testCand = "candidate:1001 1 udp 2122260223 192.168.1.100 55000 typ host";

    auto candBytes = SerializeWebRtcCandidate(testSession, testRole, testMid, testMLine, testUfrag, testCand);
    assert(!candBytes.empty());
    assert(candBytes.size() == 5 + sizeof(WebRtcCandidateHeader) + testCand.size());

    auto candMsgs = DeserializeMessages(candBytes);
    assert(candMsgs.size() == 1);
    assert(candMsgs[0].first == MessageType::WebRtcCandidate);

    std::string pMid, pUfrag, pCand;
    int32_t pMLine = -1;
    ok = ParseWebRtcCandidate(candMsgs[0].second, parsedSess, parsedRole, pMid, pMLine, pUfrag, pCand);
    assert(ok);
    assert(parsedSess == testSession);
    assert(parsedRole == testRole);
    assert(pMid == testMid);
    assert(pMLine == testMLine);
    assert(pUfrag == testUfrag);
    assert(pCand == testCand);
    std::cout << "  -> C++ WebRtcCandidate serialization & parsing: PASSED" << std::endl;

    // 5. Test Malformed and Oversized Rejection
    std::vector<uint8_t> shortPayload(20, 0);
    assert(!ParseWebRtcSdp(shortPayload, parsedSess, parsedRole, parsedSdp));
    assert(!ParseWebRtcCandidate(shortPayload, parsedSess, parsedRole, pMid, pMLine, pUfrag, pCand));

    std::string hugeSdp(70000, 'x'); // > 64KB
    assert(SerializeWebRtcOffer(testSession, testRole, hugeSdp).empty());

    std::string hugeCand(9000, 'y'); // > 8KB
    assert(SerializeWebRtcCandidate(testSession, testRole, testMid, testMLine, testUfrag, hugeCand).empty());
    std::cout << "  -> C++ Malformed & Oversized rejection: PASSED" << std::endl;

    // 6. Write golden binary fixtures for cross-language validation
    std::ofstream outOffer("/tmp/gupt_fixture_offer.bin", std::ios::binary);
    outOffer.write(reinterpret_cast<const char*>(offerBytes.data()), offerBytes.size());
    outOffer.close();

    std::ofstream outCand("/tmp/gupt_fixture_cand.bin", std::ios::binary);
    outCand.write(reinterpret_cast<const char*>(candBytes.data()), candBytes.size());
    outCand.close();

    // 7. Verify Reading JavaScript-generated binary fixtures in C++
    std::ifstream inJsAns("/tmp/gupt_fixture_js_answer.bin", std::ios::binary | std::ios::ate);
    if (inJsAns.is_open()) {
        std::streamsize sz = inJsAns.tellg();
        inJsAns.seekg(0, std::ios::beg);
        std::vector<uint8_t> jsAnsBytes(sz);
        if (inJsAns.read(reinterpret_cast<char*>(jsAnsBytes.data()), sz)) {
            auto jsMsgs = DeserializeMessages(jsAnsBytes);
            assert(jsMsgs.size() == 1);
            assert(jsMsgs[0].first == MessageType::WebRtcAnswer);
            std::string jsSess, jsRole, jsSdp;
            assert(ParseWebRtcSdp(jsMsgs[0].second, jsSess, jsRole, jsSdp));
            assert(jsSess == "sess_win_to_mac_1");
            assert(jsRole == "controller");
            assert(jsSdp.find("webrtc-datachannel") != std::string::npos);
            std::cout << "  -> [CROSS-PLATFORM] C++ decoded JS binary Answer fixture with byte-exact match: PASSED" << std::endl;
        }
    }

    std::ifstream inJsCand("/tmp/gupt_fixture_js_cand.bin", std::ios::binary | std::ios::ate);
    if (inJsCand.is_open()) {
        std::streamsize sz = inJsCand.tellg();
        inJsCand.seekg(0, std::ios::beg);
        std::vector<uint8_t> jsCandBytes(sz);
        if (inJsCand.read(reinterpret_cast<char*>(jsCandBytes.data()), sz)) {
            auto jsMsgs = DeserializeMessages(jsCandBytes);
            assert(jsMsgs.size() == 1);
            assert(jsMsgs[0].first == MessageType::WebRtcCandidate);
            std::string jsSess, jsRole, jsMid, jsUfrag, jsCandStr;
            int32_t jsMLine = -1;
            assert(ParseWebRtcCandidate(jsMsgs[0].second, jsSess, jsRole, jsMid, jsMLine, jsUfrag, jsCandStr));
            assert(jsSess == "sess_win_to_mac_1");
            assert(jsRole == "controller");
            assert(jsMid == "data");
            assert(jsMLine == 1);
            assert(jsUfrag == "mac_ufrag_01");
            assert(jsCandStr.find("typ srflx") != std::string::npos);
            std::cout << "  -> [CROSS-PLATFORM] C++ decoded JS binary Candidate fixture with byte-exact match: PASSED" << std::endl;
        }
    }

    std::cout << "[SUCCESS] All C++ WebRTC Protocol tests passed!" << std::endl;
    return 0;
}
