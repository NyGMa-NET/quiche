// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/masque/masque_client_tools.h"

#include "quiche/quic/masque/masque_encapsulated_client.h"
#include "quiche/quic/masque/masque_utils.h"
#include "quiche/quic/platform/api/quic_default_proof_providers.h"
#include "quiche/quic/tools/fake_proof_verifier.h"
#include "quiche/quic/tools/quic_name_lookup.h"
#include "quiche/quic/tools/quic_url.h"
#include "quiche/spdy/core/http2_header_block.h"

namespace quic {
namespace tools {

bool SendEncapsulatedMasqueRequest(MasqueClient* masque_client,
                                   QuicEventLoop* event_loop,
                                   std::string url_string,
                                   bool disable_certificate_verification,
                                   int address_family_for_lookup,
                                   bool dns_on_client) {
  const QuicUrl url(url_string, "https");
  std::unique_ptr<ProofVerifier> proof_verifier;
  if (disable_certificate_verification) {
    proof_verifier = std::make_unique<FakeProofVerifier>();
  } else {
    proof_verifier = CreateDefaultProofVerifier(url.host());
  }

  // Build the client, and try to connect.
  QuicSocketAddress addr;
  if (dns_on_client) {
    addr = LookupAddress(address_family_for_lookup, url.host(),
                         absl::StrCat(url.port()));
    if (!addr.IsInitialized()) {
      QUIC_LOG(ERROR) << "Unable to resolve address: " << url.host();
      return false;
    }
  } else {
    addr = QuicSocketAddress(
        masque_client->masque_client_session()->GetFakeAddress(url.host()),
        url.port());
    QUICHE_CHECK(addr.IsInitialized());
  }
  const QuicServerId server_id(url.host(), url.port());
  auto client = std::make_unique<MasqueEncapsulatedClient>(
      addr, server_id, event_loop, std::move(proof_verifier), masque_client);

  if (client == nullptr) {
    QUIC_LOG(ERROR) << "Failed to create MasqueEncapsulatedClient for "
                    << url_string;
    return false;
  }

  client->set_initial_max_packet_length(kMasqueMaxEncapsulatedPacketSize);
  client->set_drop_response_body(false);
  if (!client->Initialize()) {
    QUIC_LOG(ERROR) << "Failed to initialize MasqueEncapsulatedClient for "
                    << url_string;
    return false;
  }

  if (!client->Connect()) {
    QuicErrorCode error = client->session()->error();
    QUIC_LOG(ERROR) << "Failed to connect with client "
                    << client->session()->connection()->client_connection_id()
                    << " server " << client->session()->connection_id()
                    << " to " << url.HostPort()
                    << ". Error: " << QuicErrorCodeToString(error);
    return false;
  }

  QUIC_LOG(INFO) << "Connected client "
                 << client->session()->connection()->client_connection_id()
                 << " server " << client->session()->connection_id() << " for "
                 << url_string;

  // Construct the string body from flags, if provided.
  // TODO(dschinazi) Add support for HTTP POST and non-empty bodies.
  const std::string body = "";

  // Construct a GET or POST request for supplied URL.
  spdy::Http2HeaderBlock header_block;
  header_block[":method"] = "GET";
  header_block[":scheme"] = url.scheme();
  header_block[":authority"] = url.HostPort();
  header_block[":path"] = url.PathParamsQuery();

  // Make sure to store the response, for later output.
  client->set_store_response(true);

  // Send the MASQUE init request.
  client->SendRequestAndWaitForResponse(header_block, body,
                                        /*fin=*/true);

  if (!client->connected()) {
    QUIC_LOG(ERROR) << "Request for " << url_string
                    << " caused connection failure. Error: "
                    << QuicErrorCodeToString(client->session()->error());
    return false;
  }

  const int response_code = client->latest_response_code();
  if (response_code < 200 || response_code >= 300) {
    QUIC_LOG(ERROR) << "Request for " << url_string
                    << " failed with HTTP response code " << response_code;
    return false;
  }

  const std::string response_body = client->latest_response_body();
  QUIC_LOG(INFO) << "Request succeeded for " << url_string << std::endl
                 << response_body;

  return true;
}

}  // namespace tools
}  // namespace quic
