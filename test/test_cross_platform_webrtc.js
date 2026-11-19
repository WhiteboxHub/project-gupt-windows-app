import fs from "fs";
import assert from "assert";
import {
  MessageType,
  decodeMessages,
  encodeWebRtcOffer,
  encodeWebRtcAnswer,
  decodeWebRtcSdp,
  encodeWebRtcCandidate,
  decodeWebRtcCandidate
} from "../CrossPlatform/apps/gupt-desktop/src/protocol.js";

console.log("[TEST] Running JavaScript / Node.js WebRTC Protocol unit & cross-platform tests...");

// 1. JS-Native Offer / Answer encoding & decoding
const testSession = "sess_win_to_mac_1";
const testRole = "host";
const testSdp = "v=0\r\no=- 12345 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n";

const offerPkt = encodeWebRtcOffer(testSession, testRole, testSdp);
const decodedOfferMsgs = decodeMessages(offerPkt);
assert.strictEqual(decodedOfferMsgs.length, 1);
assert.strictEqual(decodedOfferMsgs[0].type, MessageType.WebRtcOffer);

const decodedOffer = decodeWebRtcSdp(decodedOfferMsgs[0].payload);
assert.notStrictEqual(decodedOffer, null);
assert.strictEqual(decodedOffer.sessionId, testSession);
assert.strictEqual(decodedOffer.role, testRole);
assert.strictEqual(decodedOffer.sdp, testSdp);
console.log("  -> JS-native WebRtcOffer encode & decode: PASSED");

// 2. JS-Native Candidate encoding & decoding
const testMid = "0";
const testMLine = 0;
const testUfrag = "ice_ufrag_99";
const testCand = "candidate:1001 1 udp 2122260223 192.168.1.100 55000 typ host";

const candPkt = encodeWebRtcCandidate(testSession, testRole, testMid, testMLine, testUfrag, testCand);
const decodedCandMsgs = decodeMessages(candPkt);
assert.strictEqual(decodedCandMsgs.length, 1);
assert.strictEqual(decodedCandMsgs[0].type, MessageType.WebRtcCandidate);

const decodedCand = decodeWebRtcCandidate(decodedCandMsgs[0].payload);
assert.notStrictEqual(decodedCand, null);
assert.strictEqual(decodedCand.sessionId, testSession);
assert.strictEqual(decodedCand.role, testRole);
assert.strictEqual(decodedCand.sdpMid, testMid);
assert.strictEqual(decodedCand.sdpMLineIndex, testMLine);
assert.strictEqual(decodedCand.ufrag, testUfrag);
assert.strictEqual(decodedCand.candidate, testCand);
console.log("  -> JS-native WebRtcCandidate encode & decode: PASSED");

// 3. CROSS-PLATFORM: Read C++ Generated Golden Fixture in JavaScript!
if (fs.existsSync("/tmp/gupt_fixture_offer.bin")) {
  const cppOfferBuffer = fs.readFileSync("/tmp/gupt_fixture_offer.bin");
  const cppMsgs = decodeMessages(new Uint8Array(cppOfferBuffer));
  assert.strictEqual(cppMsgs.length, 1);
  assert.strictEqual(cppMsgs[0].type, MessageType.WebRtcOffer);

  const cppOffer = decodeWebRtcSdp(cppMsgs[0].payload);
  assert.notStrictEqual(cppOffer, null);
  assert.strictEqual(cppOffer.sessionId, testSession);
  assert.strictEqual(cppOffer.role, testRole);
  assert.strictEqual(cppOffer.sdp, testSdp);
  console.log("  -> [CROSS-PLATFORM] JS decoded C++ binary Offer fixture with byte-exact match: PASSED");
}

if (fs.existsSync("/tmp/gupt_fixture_cand.bin")) {
  const cppCandBuffer = fs.readFileSync("/tmp/gupt_fixture_cand.bin");
  const cppCandMsgs = decodeMessages(new Uint8Array(cppCandBuffer));
  assert.strictEqual(cppCandMsgs.length, 1);
  assert.strictEqual(cppCandMsgs[0].type, MessageType.WebRtcCandidate);

  const cppCand = decodeWebRtcCandidate(cppCandMsgs[0].payload);
  assert.notStrictEqual(cppCand, null);
  assert.strictEqual(cppCand.sessionId, testSession);
  assert.strictEqual(cppCand.role, testRole);
  assert.strictEqual(cppCand.sdpMid, testMid);
  assert.strictEqual(cppCand.sdpMLineIndex, testMLine);
  assert.strictEqual(cppCand.ufrag, testUfrag);
  assert.strictEqual(cppCand.candidate, testCand);
  console.log("  -> [CROSS-PLATFORM] JS decoded C++ binary Candidate fixture with byte-exact match: PASSED");
}

// 4. CROSS-PLATFORM: Write JS-encoded Answer & Candidate for C++ verification
const jsAnsRole = "controller";
const jsAnsSdp = "v=0\r\no=- 88888 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n";
const jsAnsPkt = encodeWebRtcAnswer(testSession, jsAnsRole, jsAnsSdp);
fs.writeFileSync("/tmp/gupt_fixture_js_answer.bin", Buffer.from(jsAnsPkt));

const jsCand = "candidate:2002 1 udp 1677729535 10.0.0.5 56000 typ srflx";
const jsCandPkt = encodeWebRtcCandidate(testSession, jsAnsRole, "data", 1, "mac_ufrag_01", jsCand);
fs.writeFileSync("/tmp/gupt_fixture_js_cand.bin", Buffer.from(jsCandPkt));
console.log("  -> [CROSS-PLATFORM] JS wrote fixtures for C++ verification: PASSED");

// 5. Malformed & Oversized Validation
assert.strictEqual(decodeWebRtcSdp(new Uint8Array(20)), null);
assert.strictEqual(decodeWebRtcCandidate(new Uint8Array(50)), null);
assert.throws(() => encodeWebRtcOffer(testSession, testRole, "x".repeat(70000)), /exceeds 64KB/);
assert.throws(() => encodeWebRtcCandidate(testSession, testRole, "0", 0, "", "y".repeat(9000)), /exceeds 8KB/);
console.log("  -> JS Malformed & Oversized validation: PASSED");

console.log("[SUCCESS] All JavaScript WebRTC Protocol tests passed!\n");

