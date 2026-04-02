#include "tprotocol.h"

uint32_t tprotocol_last_header_found = 0;
uint32_t tprotocol_new_header_found = 0;
TPParseStep tprotocol_nextTPstep = HEADER_DETECTION;
TransmissionProtocol tprotocol_transmission = {0};
uint16_t tprotocol_expected_payload_size = 0;
bool tprotocol_payload_overflow = false;
