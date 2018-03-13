/*
 *  Copyright 2004 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <algorithm>

#include "pc/srtpfilter.h"

#include "media/base/cryptoparams.h"
#include "media/base/fakertp.h"
#include "p2p/base/sessiondescription.h"
#include "pc/srtptestutil.h"
#include "rtc_base/buffer.h"
#include "rtc_base/byteorder.h"
#include "rtc_base/constructormagic.h"
#include "rtc_base/gunit.h"
#include "rtc_base/thread.h"

using cricket::CryptoParams;
using cricket::CS_LOCAL;
using cricket::CS_REMOTE;

namespace rtc {

static const uint8_t kTestKeyGcm128_1[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ12";
static const uint8_t kTestKeyGcm128_2[] = "21ZYXWVUTSRQPONMLKJIHGFEDCBA";
static const int kTestKeyGcm128Len = 28;  // 128 bits key + 96 bits salt.
static const uint8_t kTestKeyGcm256_1[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqr";
static const uint8_t kTestKeyGcm256_2[] =
    "rqponmlkjihgfedcbaZYXWVUTSRQPONMLKJIHGFEDCBA";
static const int kTestKeyGcm256Len = 44;  // 256 bits key + 96 bits salt.
static const std::string kTestKeyParams1 =
    "inline:WVNfX19zZW1jdGwgKCkgewkyMjA7fQp9CnVubGVz";
static const std::string kTestKeyParams2 =
    "inline:PS1uQCVeeCFCanVmcjkpPywjNWhcYD0mXXtxaVBR";
static const std::string kTestKeyParams3 =
    "inline:1234X19zZW1jdGwgKCkgewkyMjA7fQp9CnVubGVz";
static const std::string kTestKeyParams4 =
    "inline:4567QCVeeCFCanVmcjkpPywjNWhcYD0mXXtxaVBR";
static const std::string kTestKeyParamsGcm1 =
    "inline:e166KFlKzJsGW0d5apX+rrI05vxbrvMJEzFI14aTDCa63IRTlLK4iH66uOI=";
static const std::string kTestKeyParamsGcm2 =
    "inline:6X0oCd55zfz4VgtOwsuqcFq61275PDYN5uwuu3p7ZUHbfUY2FMpdP4m2PEo=";
static const std::string kTestKeyParamsGcm3 =
    "inline:YKlABGZWMgX32xuMotrG0v0T7G83veegaVzubQ==";
static const std::string kTestKeyParamsGcm4 =
    "inline:gJ6tWoUym2v+/F6xjr7xaxiS3QbJJozl3ZD/0A==";
static const cricket::CryptoParams kTestCryptoParams1(
    1, "AES_CM_128_HMAC_SHA1_80", kTestKeyParams1, "");
static const cricket::CryptoParams kTestCryptoParams2(
    1, "AES_CM_128_HMAC_SHA1_80", kTestKeyParams2, "");
static const cricket::CryptoParams kTestCryptoParamsGcm1(
    1, "AEAD_AES_256_GCM", kTestKeyParamsGcm1, "");
static const cricket::CryptoParams kTestCryptoParamsGcm2(
    1, "AEAD_AES_256_GCM", kTestKeyParamsGcm2, "");
static const cricket::CryptoParams kTestCryptoParamsGcm3(
    1, "AEAD_AES_128_GCM", kTestKeyParamsGcm3, "");
static const cricket::CryptoParams kTestCryptoParamsGcm4(
    1, "AEAD_AES_128_GCM", kTestKeyParamsGcm4, "");

class SrtpFilterTest : public testing::Test {
 protected:
  SrtpFilterTest()
      // Need to initialize |sequence_number_|, the value does not matter.
      : sequence_number_(1) {}
  static std::vector<CryptoParams> MakeVector(const CryptoParams& params) {
    std::vector<CryptoParams> vec;
    vec.push_back(params);
    return vec;
  }
  void TestSetParams(const std::vector<CryptoParams>& params1,
                     const std::vector<CryptoParams>& params2) {
    EXPECT_TRUE(f1_.SetOffer(params1, CS_LOCAL));
    EXPECT_TRUE(f2_.SetOffer(params1, CS_REMOTE));
    EXPECT_FALSE(f1_.IsActive());
    EXPECT_FALSE(f2_.IsActive());
    EXPECT_TRUE(f2_.SetAnswer(params2, CS_LOCAL));
    EXPECT_TRUE(f1_.SetAnswer(params2, CS_REMOTE));
    EXPECT_TRUE(f1_.IsActive());
    EXPECT_TRUE(f2_.IsActive());
  }
  void TestRtpAuthParams(cricket::SrtpFilter* filter, const std::string& cs) {
    int overhead;
    EXPECT_TRUE(filter->GetSrtpOverhead(&overhead));
    switch (SrtpCryptoSuiteFromName(cs)) {
      case SRTP_AES128_CM_SHA1_32:
        EXPECT_EQ(32 / 8, overhead);  // 32-bit tag.
        break;
      case SRTP_AES128_CM_SHA1_80:
        EXPECT_EQ(80 / 8, overhead);  // 80-bit tag.
        break;
      default:
        RTC_NOTREACHED();
        break;
    }

    uint8_t* auth_key = nullptr;
    int key_len = 0;
    int tag_len = 0;
    EXPECT_TRUE(filter->GetRtpAuthParams(&auth_key, &key_len, &tag_len));
    EXPECT_NE(nullptr, auth_key);
    EXPECT_EQ(160 / 8, key_len);  // Length of SHA-1 is 160 bits.
    EXPECT_EQ(overhead, tag_len);
  }
  void TestProtectUnprotect(const std::string& cs1, const std::string& cs2) {
    Buffer rtp_buffer(sizeof(kPcmuFrame) + rtp_auth_tag_len(cs1));
    char* rtp_packet = rtp_buffer.data<char>();
    char original_rtp_packet[sizeof(kPcmuFrame)];
    Buffer rtcp_buffer(sizeof(kRtcpReport) + 4 + rtcp_auth_tag_len(cs2));
    char* rtcp_packet = rtcp_buffer.data<char>();
    int rtp_len = sizeof(kPcmuFrame), rtcp_len = sizeof(kRtcpReport), out_len;
    memcpy(rtp_packet, kPcmuFrame, rtp_len);
    // In order to be able to run this test function multiple times we can not
    // use the same sequence number twice. Increase the sequence number by one.
    SetBE16(reinterpret_cast<uint8_t*>(rtp_packet) + 2, ++sequence_number_);
    memcpy(original_rtp_packet, rtp_packet, rtp_len);
    memcpy(rtcp_packet, kRtcpReport, rtcp_len);

    EXPECT_TRUE(f1_.ProtectRtp(rtp_packet, rtp_len,
                               static_cast<int>(rtp_buffer.size()), &out_len));
    EXPECT_EQ(out_len, rtp_len + rtp_auth_tag_len(cs1));
    EXPECT_NE(0, memcmp(rtp_packet, original_rtp_packet, rtp_len));
    if (!f1_.IsExternalAuthActive()) {
      EXPECT_TRUE(f2_.UnprotectRtp(rtp_packet, out_len, &out_len));
      EXPECT_EQ(rtp_len, out_len);
      EXPECT_EQ(0, memcmp(rtp_packet, original_rtp_packet, rtp_len));
    } else {
      // With external auth enabled, SRTP doesn't write the auth tag and
      // unprotect would fail. Check accessing the information about the
      // tag instead, similar to what the actual code would do that relies
      // on external auth.
      TestRtpAuthParams(&f1_, cs1);
    }

    EXPECT_TRUE(f2_.ProtectRtp(rtp_packet, rtp_len,
                               static_cast<int>(rtp_buffer.size()), &out_len));
    EXPECT_EQ(out_len, rtp_len + rtp_auth_tag_len(cs2));
    EXPECT_NE(0, memcmp(rtp_packet, original_rtp_packet, rtp_len));
    if (!f2_.IsExternalAuthActive()) {
      EXPECT_TRUE(f1_.UnprotectRtp(rtp_packet, out_len, &out_len));
      EXPECT_EQ(rtp_len, out_len);
      EXPECT_EQ(0, memcmp(rtp_packet, original_rtp_packet, rtp_len));
    } else {
      TestRtpAuthParams(&f2_, cs2);
    }

    EXPECT_TRUE(f1_.ProtectRtcp(
        rtcp_packet, rtcp_len, static_cast<int>(rtcp_buffer.size()), &out_len));
    EXPECT_EQ(out_len, rtcp_len + 4 + rtcp_auth_tag_len(cs1));  // NOLINT
    EXPECT_NE(0, memcmp(rtcp_packet, kRtcpReport, rtcp_len));
    EXPECT_TRUE(f2_.UnprotectRtcp(rtcp_packet, out_len, &out_len));
    EXPECT_EQ(rtcp_len, out_len);
    EXPECT_EQ(0, memcmp(rtcp_packet, kRtcpReport, rtcp_len));

    EXPECT_TRUE(f2_.ProtectRtcp(
        rtcp_packet, rtcp_len, static_cast<int>(rtcp_buffer.size()), &out_len));
    EXPECT_EQ(out_len, rtcp_len + 4 + rtcp_auth_tag_len(cs2));  // NOLINT
    EXPECT_NE(0, memcmp(rtcp_packet, kRtcpReport, rtcp_len));
    EXPECT_TRUE(f1_.UnprotectRtcp(rtcp_packet, out_len, &out_len));
    EXPECT_EQ(rtcp_len, out_len);
    EXPECT_EQ(0, memcmp(rtcp_packet, kRtcpReport, rtcp_len));
  }
  void TestProtectUnprotectHeaderEncryption(
      const std::string& cs1,
      const std::string& cs2,
      const std::vector<int>& encrypted_header_ids) {
    Buffer rtp_buffer(sizeof(kPcmuFrameWithExtensions) + rtp_auth_tag_len(cs1));
    char* rtp_packet = rtp_buffer.data<char>();
    size_t rtp_packet_size = rtp_buffer.size();
    char original_rtp_packet[sizeof(kPcmuFrameWithExtensions)];
    size_t original_rtp_packet_size = sizeof(original_rtp_packet);
    int rtp_len = sizeof(kPcmuFrameWithExtensions), out_len;
    memcpy(rtp_packet, kPcmuFrameWithExtensions, rtp_len);
    // In order to be able to run this test function multiple times we can not
    // use the same sequence number twice. Increase the sequence number by one.
    SetBE16(reinterpret_cast<uint8_t*>(rtp_packet) + 2, ++sequence_number_);
    memcpy(original_rtp_packet, rtp_packet, rtp_len);

    EXPECT_TRUE(f1_.ProtectRtp(rtp_packet, rtp_len,
                               static_cast<int>(rtp_buffer.size()), &out_len));
    EXPECT_EQ(out_len, rtp_len + rtp_auth_tag_len(cs1));
    EXPECT_NE(0, memcmp(rtp_packet, original_rtp_packet, rtp_len));
    CompareHeaderExtensions(rtp_packet, rtp_packet_size, original_rtp_packet,
                            original_rtp_packet_size, encrypted_header_ids,
                            false);
    EXPECT_TRUE(f2_.UnprotectRtp(rtp_packet, out_len, &out_len));
    EXPECT_EQ(rtp_len, out_len);
    EXPECT_EQ(0, memcmp(rtp_packet, original_rtp_packet, rtp_len));
    CompareHeaderExtensions(rtp_packet, rtp_packet_size, original_rtp_packet,
                            original_rtp_packet_size, encrypted_header_ids,
                            true);

    EXPECT_TRUE(f2_.ProtectRtp(rtp_packet, rtp_len,
                               static_cast<int>(rtp_buffer.size()), &out_len));
    EXPECT_EQ(out_len, rtp_len + rtp_auth_tag_len(cs2));
    EXPECT_NE(0, memcmp(rtp_packet, original_rtp_packet, rtp_len));
    CompareHeaderExtensions(rtp_packet, rtp_packet_size, original_rtp_packet,
                            original_rtp_packet_size, encrypted_header_ids,
                            false);
    EXPECT_TRUE(f1_.UnprotectRtp(rtp_packet, out_len, &out_len));
    EXPECT_EQ(rtp_len, out_len);
    EXPECT_EQ(0, memcmp(rtp_packet, original_rtp_packet, rtp_len));
    CompareHeaderExtensions(rtp_packet, rtp_packet_size, original_rtp_packet,
                            original_rtp_packet_size, encrypted_header_ids,
                            true);
  }
  void TestProtectSetParamsDirect(bool enable_external_auth,
                                  int cs,
                                  const uint8_t* key1,
                                  int key1_len,
                                  const uint8_t* key2,
                                  int key2_len,
                                  const std::string& cs_name) {
    EXPECT_EQ(key1_len, key2_len);
    EXPECT_EQ(cs_name, SrtpCryptoSuiteToName(cs));
    if (enable_external_auth) {
      f1_.EnableExternalAuth();
      f2_.EnableExternalAuth();
    }
    EXPECT_TRUE(f1_.SetRtpParams(cs, key1, key1_len, cs, key2, key2_len));
    EXPECT_TRUE(f2_.SetRtpParams(cs, key2, key2_len, cs, key1, key1_len));
    EXPECT_TRUE(f1_.SetRtcpParams(cs, key1, key1_len, cs, key2, key2_len));
    EXPECT_TRUE(f2_.SetRtcpParams(cs, key2, key2_len, cs, key1, key1_len));
    EXPECT_TRUE(f1_.IsActive());
    EXPECT_TRUE(f2_.IsActive());
    if (IsGcmCryptoSuite(cs)) {
      EXPECT_FALSE(f1_.IsExternalAuthActive());
      EXPECT_FALSE(f2_.IsExternalAuthActive());
    } else if (enable_external_auth) {
      EXPECT_TRUE(f1_.IsExternalAuthActive());
      EXPECT_TRUE(f2_.IsExternalAuthActive());
    }
    TestProtectUnprotect(cs_name, cs_name);
  }
  void TestProtectSetParamsDirectHeaderEncryption(int cs,
                                                  const uint8_t* key1,
                                                  int key1_len,
                                                  const uint8_t* key2,
                                                  int key2_len,
                                                  const std::string& cs_name) {
    std::vector<int> encrypted_headers;
    encrypted_headers.push_back(1);
    // Don't encrypt header ids 2 and 3.
    encrypted_headers.push_back(4);
    EXPECT_EQ(key1_len, key2_len);
    EXPECT_EQ(cs_name, SrtpCryptoSuiteToName(cs));
    f1_.SetEncryptedHeaderExtensionIds(CS_LOCAL, encrypted_headers);
    f1_.SetEncryptedHeaderExtensionIds(CS_REMOTE, encrypted_headers);
    f2_.SetEncryptedHeaderExtensionIds(CS_LOCAL, encrypted_headers);
    f2_.SetEncryptedHeaderExtensionIds(CS_REMOTE, encrypted_headers);
    EXPECT_TRUE(f1_.SetRtpParams(cs, key1, key1_len, cs, key2, key2_len));
    EXPECT_TRUE(f2_.SetRtpParams(cs, key2, key2_len, cs, key1, key1_len));
    EXPECT_TRUE(f1_.IsActive());
    EXPECT_TRUE(f2_.IsActive());
    EXPECT_FALSE(f1_.IsExternalAuthActive());
    EXPECT_FALSE(f2_.IsExternalAuthActive());
    TestProtectUnprotectHeaderEncryption(cs_name, cs_name, encrypted_headers);
  }
  cricket::SrtpFilter f1_;
  cricket::SrtpFilter f2_;
  int sequence_number_;
};

// Test that we can set up the session and keys properly.
TEST_F(SrtpFilterTest, TestGoodSetupOneCipherSuite) {
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParams1), CS_LOCAL));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_TRUE(f1_.SetAnswer(MakeVector(kTestCryptoParams2), CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
}

TEST_F(SrtpFilterTest, TestGoodSetupOneCipherSuiteGcm) {
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParamsGcm1), CS_LOCAL));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_TRUE(f1_.SetAnswer(MakeVector(kTestCryptoParamsGcm2), CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
}

// Test that we can set up things with multiple params.
TEST_F(SrtpFilterTest, TestGoodSetupMultipleCipherSuites) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  offer.push_back(kTestCryptoParams1);
  offer[1].tag = 2;
  offer[1].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;
  answer[0].tag = 2;
  answer[0].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_TRUE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
}

TEST_F(SrtpFilterTest, TestGoodSetupMultipleCipherSuitesGcm) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParamsGcm1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParamsGcm3));
  offer.push_back(kTestCryptoParamsGcm4);
  offer[1].tag = 2;
  answer[0].tag = 2;
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_TRUE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
}

// Test that we handle the cases where crypto is not desired.
TEST_F(SrtpFilterTest, TestGoodSetupNoCipherSuites) {
  std::vector<CryptoParams> offer, answer;
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_TRUE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we handle the cases where crypto is not desired by the remote side.
TEST_F(SrtpFilterTest, TestGoodSetupNoAnswerCipherSuites) {
  std::vector<CryptoParams> answer;
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParams1), CS_LOCAL));
  EXPECT_TRUE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail if we call the functions the wrong way.
TEST_F(SrtpFilterTest, TestBadSetup) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_LOCAL));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we can set offer multiple times from the same source.
TEST_F(SrtpFilterTest, TestGoodSetupMultipleOffers) {
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParams1), CS_LOCAL));
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParams2), CS_LOCAL));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_TRUE(f1_.SetAnswer(MakeVector(kTestCryptoParams2), CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParams1), CS_LOCAL));
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParams2), CS_LOCAL));
  EXPECT_TRUE(f1_.SetAnswer(MakeVector(kTestCryptoParams2), CS_REMOTE));

  EXPECT_TRUE(f2_.SetOffer(MakeVector(kTestCryptoParams1), CS_REMOTE));
  EXPECT_TRUE(f2_.SetOffer(MakeVector(kTestCryptoParams2), CS_REMOTE));
  EXPECT_FALSE(f2_.IsActive());
  EXPECT_TRUE(f2_.SetAnswer(MakeVector(kTestCryptoParams2), CS_LOCAL));
  EXPECT_TRUE(f2_.IsActive());
  EXPECT_TRUE(f2_.SetOffer(MakeVector(kTestCryptoParams1), CS_REMOTE));
  EXPECT_TRUE(f2_.SetOffer(MakeVector(kTestCryptoParams2), CS_REMOTE));
  EXPECT_TRUE(f2_.SetAnswer(MakeVector(kTestCryptoParams2), CS_LOCAL));
}
// Test that we can't set offer multiple times from different sources.
TEST_F(SrtpFilterTest, TestBadSetupMultipleOffers) {
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParams1), CS_LOCAL));
  EXPECT_FALSE(f1_.SetOffer(MakeVector(kTestCryptoParams2), CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_TRUE(f1_.SetAnswer(MakeVector(kTestCryptoParams1), CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParams2), CS_LOCAL));
  EXPECT_FALSE(f1_.SetOffer(MakeVector(kTestCryptoParams1), CS_REMOTE));
  EXPECT_TRUE(f1_.SetAnswer(MakeVector(kTestCryptoParams2), CS_REMOTE));

  EXPECT_TRUE(f2_.SetOffer(MakeVector(kTestCryptoParams2), CS_REMOTE));
  EXPECT_FALSE(f2_.SetOffer(MakeVector(kTestCryptoParams1), CS_LOCAL));
  EXPECT_FALSE(f2_.IsActive());
  EXPECT_TRUE(f2_.SetAnswer(MakeVector(kTestCryptoParams2), CS_LOCAL));
  EXPECT_TRUE(f2_.IsActive());
  EXPECT_TRUE(f2_.SetOffer(MakeVector(kTestCryptoParams2), CS_REMOTE));
  EXPECT_FALSE(f2_.SetOffer(MakeVector(kTestCryptoParams1), CS_LOCAL));
  EXPECT_TRUE(f2_.SetAnswer(MakeVector(kTestCryptoParams2), CS_LOCAL));
}

// Test that we fail if we have params in the answer when none were offered.
TEST_F(SrtpFilterTest, TestNoAnswerCipherSuites) {
  std::vector<CryptoParams> offer;
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(MakeVector(kTestCryptoParams2), CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail if we have too many params in our answer.
TEST_F(SrtpFilterTest, TestMultipleAnswerCipherSuites) {
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  answer.push_back(kTestCryptoParams2);
  answer[1].tag = 2;
  answer[1].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;
  EXPECT_TRUE(f1_.SetOffer(MakeVector(kTestCryptoParams1), CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail if we don't support the cipher-suite.
TEST_F(SrtpFilterTest, TestInvalidCipherSuite) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  offer[0].cipher_suite = answer[0].cipher_suite = "FOO";
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail if we can't agree on a tag.
TEST_F(SrtpFilterTest, TestNoMatchingTag) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  answer[0].tag = 99;
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail if we can't agree on a cipher-suite.
TEST_F(SrtpFilterTest, TestNoMatchingCipherSuite) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  answer[0].tag = 2;
  answer[0].cipher_suite = "FOO";
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail keys with bad base64 content.
TEST_F(SrtpFilterTest, TestInvalidKeyData) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  answer[0].key_params = "inline:!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!";
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail keys with the wrong key-method.
TEST_F(SrtpFilterTest, TestWrongKeyMethod) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  answer[0].key_params = "outline:PS1uQCVeeCFCanVmcjkpPywjNWhcYD0mXXtxaVBR";
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail keys of the wrong length.
TEST_F(SrtpFilterTest, TestKeyTooShort) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  answer[0].key_params = "inline:PS1uQCVeeCFCanVmcjkpPywjNWhcYD0mXXtx";
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail keys of the wrong length.
TEST_F(SrtpFilterTest, TestKeyTooLong) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  answer[0].key_params = "inline:PS1uQCVeeCFCanVmcjkpPywjNWhcYD0mXXtxaVBRABCD";
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we fail keys with lifetime or MKI set (since we don't support)
TEST_F(SrtpFilterTest, TestUnsupportedOptions) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  answer[0].key_params =
      "inline:PS1uQCVeeCFCanVmcjkpPywjNWhcYD0mXXtxaVBR|2^20|1:4";
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
}

// Test that we can encrypt/decrypt after setting the same CryptoParams again on
// one side.
TEST_F(SrtpFilterTest, TestSettingSameKeyOnOneSide) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  TestSetParams(offer, answer);

  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);

  // Re-applying the same keys on one end and it should not reset the ROC.
  EXPECT_TRUE(f2_.SetOffer(offer, CS_REMOTE));
  EXPECT_TRUE(f2_.SetAnswer(answer, CS_LOCAL));
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);
}

// Test that we can encrypt/decrypt after negotiating AES_CM_128_HMAC_SHA1_80.
TEST_F(SrtpFilterTest, TestProtect_AES_CM_128_HMAC_SHA1_80) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  offer.push_back(kTestCryptoParams1);
  offer[1].tag = 2;
  offer[1].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;
  TestSetParams(offer, answer);
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);
}

// Test that we can encrypt/decrypt after negotiating AES_CM_128_HMAC_SHA1_32.
TEST_F(SrtpFilterTest, TestProtect_AES_CM_128_HMAC_SHA1_32) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));
  offer.push_back(kTestCryptoParams1);
  offer[1].tag = 2;
  offer[1].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;
  answer[0].tag = 2;
  answer[0].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;
  TestSetParams(offer, answer);
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_32, CS_AES_CM_128_HMAC_SHA1_32);
}

// Test that we can change encryption parameters.
TEST_F(SrtpFilterTest, TestChangeParameters) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));

  TestSetParams(offer, answer);
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);

  // Change the key parameters and cipher_suite.
  offer[0].key_params = kTestKeyParams3;
  offer[0].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;
  answer[0].key_params = kTestKeyParams4;
  answer[0].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;

  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_TRUE(f2_.SetOffer(offer, CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
  EXPECT_TRUE(f1_.IsActive());

  // Test that the old keys are valid until the negotiation is complete.
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);

  // Complete the negotiation and test that we can still understand each other.
  EXPECT_TRUE(f2_.SetAnswer(answer, CS_LOCAL));
  EXPECT_TRUE(f1_.SetAnswer(answer, CS_REMOTE));

  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_32, CS_AES_CM_128_HMAC_SHA1_32);
}

// Test that we can send and receive provisional answers with crypto enabled.
// Also test that we can change the crypto.
TEST_F(SrtpFilterTest, TestProvisionalAnswer) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  offer.push_back(kTestCryptoParams1);
  offer[1].tag = 2;
  offer[1].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));

  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_TRUE(f2_.SetOffer(offer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_FALSE(f2_.IsActive());
  EXPECT_TRUE(f2_.SetProvisionalAnswer(answer, CS_LOCAL));
  EXPECT_TRUE(f1_.SetProvisionalAnswer(answer, CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
  EXPECT_TRUE(f2_.IsActive());
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);

  answer[0].key_params = kTestKeyParams4;
  answer[0].tag = 2;
  answer[0].cipher_suite = CS_AES_CM_128_HMAC_SHA1_32;
  EXPECT_TRUE(f2_.SetAnswer(answer, CS_LOCAL));
  EXPECT_TRUE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
  EXPECT_TRUE(f2_.IsActive());
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_32, CS_AES_CM_128_HMAC_SHA1_32);
}

// Test that a provisional answer doesn't need to contain a crypto.
TEST_F(SrtpFilterTest, TestProvisionalAnswerWithoutCrypto) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer;

  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_TRUE(f2_.SetOffer(offer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_FALSE(f2_.IsActive());
  EXPECT_TRUE(f2_.SetProvisionalAnswer(answer, CS_LOCAL));
  EXPECT_TRUE(f1_.SetProvisionalAnswer(answer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_FALSE(f2_.IsActive());

  answer.push_back(kTestCryptoParams2);
  EXPECT_TRUE(f2_.SetAnswer(answer, CS_LOCAL));
  EXPECT_TRUE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
  EXPECT_TRUE(f2_.IsActive());
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);
}

// Test that if we get a new local offer after a provisional answer
// with no crypto, that we are in an inactive state.
TEST_F(SrtpFilterTest, TestLocalOfferAfterProvisionalAnswerWithoutCrypto) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer;

  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_TRUE(f2_.SetOffer(offer, CS_REMOTE));
  EXPECT_TRUE(f1_.SetProvisionalAnswer(answer, CS_REMOTE));
  EXPECT_TRUE(f2_.SetProvisionalAnswer(answer, CS_LOCAL));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_FALSE(f2_.IsActive());
  // The calls to set an offer after a provisional answer fail, so the
  // state doesn't change.
  EXPECT_FALSE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_FALSE(f2_.SetOffer(offer, CS_REMOTE));
  EXPECT_FALSE(f1_.IsActive());
  EXPECT_FALSE(f2_.IsActive());

  answer.push_back(kTestCryptoParams2);
  EXPECT_TRUE(f2_.SetAnswer(answer, CS_LOCAL));
  EXPECT_TRUE(f1_.SetAnswer(answer, CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
  EXPECT_TRUE(f2_.IsActive());
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);
}

// Test that we can disable encryption.
TEST_F(SrtpFilterTest, TestDisableEncryption) {
  std::vector<CryptoParams> offer(MakeVector(kTestCryptoParams1));
  std::vector<CryptoParams> answer(MakeVector(kTestCryptoParams2));

  TestSetParams(offer, answer);
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);

  offer.clear();
  answer.clear();
  EXPECT_TRUE(f1_.SetOffer(offer, CS_LOCAL));
  EXPECT_TRUE(f2_.SetOffer(offer, CS_REMOTE));
  EXPECT_TRUE(f1_.IsActive());
  EXPECT_TRUE(f2_.IsActive());

  // Test that the old keys are valid until the negotiation is complete.
  TestProtectUnprotect(CS_AES_CM_128_HMAC_SHA1_80, CS_AES_CM_128_HMAC_SHA1_80);

  // Complete the negotiation.
  EXPECT_TRUE(f2_.SetAnswer(answer, CS_LOCAL));
  EXPECT_TRUE(f1_.SetAnswer(answer, CS_REMOTE));

  EXPECT_FALSE(f1_.IsActive());
  EXPECT_FALSE(f2_.IsActive());
}

class SrtpFilterProtectSetParamsDirectTest
    : public SrtpFilterTest,
      public testing::WithParamInterface<bool> {};

// Test directly setting the params with AES_CM_128_HMAC_SHA1_80.
TEST_P(SrtpFilterProtectSetParamsDirectTest, Test_AES_CM_128_HMAC_SHA1_80) {
  bool enable_external_auth = GetParam();
  TestProtectSetParamsDirect(enable_external_auth, SRTP_AES128_CM_SHA1_80,
                             kTestKey1, kTestKeyLen, kTestKey2, kTestKeyLen,
                             CS_AES_CM_128_HMAC_SHA1_80);
}

TEST_F(SrtpFilterTest,
       TestProtectSetParamsDirectHeaderEncryption_AES_CM_128_HMAC_SHA1_80) {
  TestProtectSetParamsDirectHeaderEncryption(
      SRTP_AES128_CM_SHA1_80, kTestKey1, kTestKeyLen, kTestKey2, kTestKeyLen,
      CS_AES_CM_128_HMAC_SHA1_80);
}

// Test directly setting the params with AES_CM_128_HMAC_SHA1_32.
TEST_P(SrtpFilterProtectSetParamsDirectTest, Test_AES_CM_128_HMAC_SHA1_32) {
  bool enable_external_auth = GetParam();
  TestProtectSetParamsDirect(enable_external_auth, SRTP_AES128_CM_SHA1_32,
                             kTestKey1, kTestKeyLen, kTestKey2, kTestKeyLen,
                             CS_AES_CM_128_HMAC_SHA1_32);
}

TEST_F(SrtpFilterTest,
       TestProtectSetParamsDirectHeaderEncryption_AES_CM_128_HMAC_SHA1_32) {
  TestProtectSetParamsDirectHeaderEncryption(
      SRTP_AES128_CM_SHA1_32, kTestKey1, kTestKeyLen, kTestKey2, kTestKeyLen,
      CS_AES_CM_128_HMAC_SHA1_32);
}

// Test directly setting the params with SRTP_AEAD_AES_128_GCM.
TEST_P(SrtpFilterProtectSetParamsDirectTest, Test_SRTP_AEAD_AES_128_GCM) {
  bool enable_external_auth = GetParam();
  TestProtectSetParamsDirect(enable_external_auth, SRTP_AEAD_AES_128_GCM,
                             kTestKeyGcm128_1, kTestKeyGcm128Len,
                             kTestKeyGcm128_2, kTestKeyGcm128Len,
                             CS_AEAD_AES_128_GCM);
}

TEST_F(SrtpFilterTest,
       TestProtectSetParamsDirectHeaderEncryption_SRTP_AEAD_AES_128_GCM) {
  TestProtectSetParamsDirectHeaderEncryption(
      SRTP_AEAD_AES_128_GCM, kTestKeyGcm128_1, kTestKeyGcm128Len,
      kTestKeyGcm128_2, kTestKeyGcm128Len, CS_AEAD_AES_128_GCM);
}

// Test directly setting the params with SRTP_AEAD_AES_256_GCM.
TEST_P(SrtpFilterProtectSetParamsDirectTest, Test_SRTP_AEAD_AES_256_GCM) {
  bool enable_external_auth = GetParam();
  TestProtectSetParamsDirect(enable_external_auth, SRTP_AEAD_AES_256_GCM,
                             kTestKeyGcm256_1, kTestKeyGcm256Len,
                             kTestKeyGcm256_2, kTestKeyGcm256Len,
                             CS_AEAD_AES_256_GCM);
}

TEST_F(SrtpFilterTest,
       TestProtectSetParamsDirectHeaderEncryption_SRTP_AEAD_AES_256_GCM) {
  TestProtectSetParamsDirectHeaderEncryption(
      SRTP_AEAD_AES_256_GCM, kTestKeyGcm256_1, kTestKeyGcm256Len,
      kTestKeyGcm256_2, kTestKeyGcm256Len, CS_AEAD_AES_256_GCM);
}

// Run all tests both with and without external auth enabled.
INSTANTIATE_TEST_CASE_P(ExternalAuth,
                        SrtpFilterProtectSetParamsDirectTest,
                        ::testing::Values(true, false));

// Test directly setting the params with bogus keys.
TEST_F(SrtpFilterTest, TestSetParamsKeyTooShort) {
  EXPECT_FALSE(f1_.SetRtpParams(SRTP_AES128_CM_SHA1_80, kTestKey1,
                                kTestKeyLen - 1, SRTP_AES128_CM_SHA1_80,
                                kTestKey1, kTestKeyLen - 1));
  EXPECT_FALSE(f1_.SetRtcpParams(SRTP_AES128_CM_SHA1_80, kTestKey1,
                                 kTestKeyLen - 1, SRTP_AES128_CM_SHA1_80,
                                 kTestKey1, kTestKeyLen - 1));
}

}  // namespace rtc
