/*
    +--------------------------------------------------------------------+
    | libmemcached-awesome - C/C++ Client Library for memcached          |
    +--------------------------------------------------------------------+
    | Redistribution and use in source and binary forms, with or without |
    | modification, are permitted under the terms of the BSD license.    |
    | You should have received a copy of the license in a bundled file   |
    | named LICENSE; in case you did not receive a copy you can review   |
    | the terms online at: https://opensource.org/licenses/BSD-3-Clause  |
    +--------------------------------------------------------------------+
    | Copyright (c) 2006-2014 Brian Aker   https://datadifferential.com/ |
    | Copyright (c) 2020-2021 Michael Wallner        https://awesome.co/ |
    +--------------------------------------------------------------------+
*/

#include "libmemcached/common.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <numeric>
#include "jerasure.h"
#include "reed_sol.h"

enum memcached_storage_action_t { SET_OP, REPLACE_OP, ADD_OP, PREPEND_OP, APPEND_OP, CAS_OP };

/* Inline this */
static inline const char *storage_op_string(memcached_storage_action_t verb) {
  switch (verb) {
  case REPLACE_OP:
    return "replace ";

  case ADD_OP:
    return "add ";

  case PREPEND_OP:
    return "prepend ";

  case APPEND_OP:
    return "append ";

  case CAS_OP:
    return "cas ";

  case SET_OP:
    break;
  }

  return "set ";
}

static inline bool can_by_encrypted(const memcached_storage_action_t verb) {
  switch (verb) {
  case SET_OP:
  case ADD_OP:
  case CAS_OP:
  case REPLACE_OP:
    return true;

  case APPEND_OP:
  case PREPEND_OP:
    break;
  }

  return false;
}

static inline uint8_t get_com_code(const memcached_storage_action_t verb, const bool reply) {
  if (reply == false) {
    switch (verb) {
    case SET_OP:
      return PROTOCOL_BINARY_CMD_SETQ;

    case ADD_OP:
      return PROTOCOL_BINARY_CMD_ADDQ;

    case CAS_OP: /* FALLTHROUGH */
    case REPLACE_OP:
      return PROTOCOL_BINARY_CMD_REPLACEQ;

    case APPEND_OP:
      return PROTOCOL_BINARY_CMD_APPENDQ;

    case PREPEND_OP:
      return PROTOCOL_BINARY_CMD_PREPENDQ;
    }
  }

  switch (verb) {
  case SET_OP:
    break;

  case ADD_OP:
    return PROTOCOL_BINARY_CMD_ADD;

  case CAS_OP: /* FALLTHROUGH */
  case REPLACE_OP:
    return PROTOCOL_BINARY_CMD_REPLACE;

  case APPEND_OP:
    return PROTOCOL_BINARY_CMD_APPEND;

  case PREPEND_OP:
    return PROTOCOL_BINARY_CMD_PREPEND;
  }

  return PROTOCOL_BINARY_CMD_SET;
}

static memcached_return_t memcached_send_binary(Memcached *ptr, memcached_instance_st *server,
                                                uint32_t server_key, const char *key,
                                                const size_t key_length, const char *value,
                                                const size_t value_length, const time_t expiration,
                                                const uint32_t flags, const uint64_t cas,
                                                const bool flush, const bool reply,
                                                memcached_storage_action_t verb) {
  protocol_binary_request_set request = {};
  size_t send_length = sizeof(request.bytes);

  initialize_binary_request(server, request.message.header);

  request.message.header.request.opcode = get_com_code(verb, reply);
  request.message.header.request.keylen =
      htons((uint16_t)(key_length + memcached_array_size(ptr->_namespace)));
  request.message.header.request.datatype = PROTOCOL_BINARY_RAW_BYTES;
  if (verb == APPEND_OP or verb == PREPEND_OP) {
    send_length -= 8; /* append & prepend does not contain extras! */
  } else {
    request.message.header.request.extlen = 8;
    request.message.body.flags = htonl(flags);
    request.message.body.expiration = htonl((uint32_t) expiration);
  }

  request.message.header.request.bodylen =
      htonl((uint32_t)(key_length + memcached_array_size(ptr->_namespace) + value_length
                       + request.message.header.request.extlen));

  if (cas) {
    request.message.header.request.cas = memcached_htonll(cas);
  }

  libmemcached_io_vector_st vector[] = {
      {NULL, 0},
      {request.bytes, send_length},
      {memcached_array_string(ptr->_namespace), memcached_array_size(ptr->_namespace)},
      {key, key_length},
      {value, value_length}};

  /* write the header */
  memcached_return_t rc;
  if ((rc = memcached_vdo(server, vector, 5, flush)) != MEMCACHED_SUCCESS) {
    assert(memcached_last_error(server->root) != MEMCACHED_SUCCESS);
    return memcached_last_error(server->root);
  }

  if (verb == SET_OP and ptr->number_of_replicas > 0) {
    request.message.header.request.opcode = PROTOCOL_BINARY_CMD_SETQ;
    WATCHPOINT_STRING("replicating");

    for (uint32_t x = 0; x < ptr->number_of_replicas; x++) {
      ++server_key;
      if (server_key == memcached_server_count(ptr)) {
        server_key = 0;
      }

      memcached_instance_st *instance = memcached_instance_fetch(ptr, server_key);

      if (memcached_success(memcached_vdo(instance, vector, 5, false))) {
        memcached_server_response_decrement(instance);
      }
    }
  }

  if (flush == false) {
    return MEMCACHED_BUFFERED;
  }

  // No reply always assumes success
  if (reply == false) {
    return MEMCACHED_SUCCESS;
  }

  return memcached_response(server, NULL, 0, NULL);
}

static memcached_return_t
memcached_send_ascii(Memcached *ptr, memcached_instance_st *instance, const char *key,
                     const size_t key_length, const char *value, const size_t value_length,
                     const time_t expiration, const uint32_t flags, const uint64_t cas,
                     const bool flush, const bool reply, const memcached_storage_action_t verb) {
  char flags_buffer[MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH + 1];
  int flags_buffer_length = snprintf(flags_buffer, sizeof(flags_buffer), " %u", flags);
  if (size_t(flags_buffer_length) >= sizeof(flags_buffer) or flags_buffer_length < 0) {
    return memcached_set_error(
        *instance, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT,
        memcached_literal_param("snprintf(MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH)"));
  }

  char expiration_buffer[MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH + 1];
  int expiration_buffer_length = snprintf(expiration_buffer, sizeof(expiration_buffer), " %llu",
                                          (unsigned long long) expiration);
  if (size_t(expiration_buffer_length) >= sizeof(expiration_buffer) or expiration_buffer_length < 0)
  {
    return memcached_set_error(
        *instance, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT,
        memcached_literal_param("snprintf(MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH)"));
  }

  char value_buffer[MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH + 1];
  int value_buffer_length =
      snprintf(value_buffer, sizeof(value_buffer), " %llu", (unsigned long long) value_length);
  if (size_t(value_buffer_length) >= sizeof(value_buffer) or value_buffer_length < 0) {
    return memcached_set_error(
        *instance, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT,
        memcached_literal_param("snprintf(MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH)"));
  }

  char cas_buffer[MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH + 1];
  int cas_buffer_length = 0;
  if (cas) {
    cas_buffer_length = snprintf(cas_buffer, sizeof(cas_buffer), " %llu", (unsigned long long) cas);
    if (size_t(cas_buffer_length) >= sizeof(cas_buffer) or cas_buffer_length < 0) {
      return memcached_set_error(
          *instance, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT,
          memcached_literal_param("snprintf(MEMCACHED_MAXIMUM_INTEGER_DISPLAY_LENGTH)"));
    }
  }

  libmemcached_io_vector_st vector[] = {
      {NULL, 0},
      {storage_op_string(verb), strlen(storage_op_string(verb))},
      {memcached_array_string(ptr->_namespace), memcached_array_size(ptr->_namespace)},
      {key, key_length},
      {flags_buffer, size_t(flags_buffer_length)},
      {expiration_buffer, size_t(expiration_buffer_length)},
      {value_buffer, size_t(value_buffer_length)},
      {cas_buffer, size_t(cas_buffer_length)},
      {" noreply", reply ? 0 : memcached_literal_param_size(" noreply")},
      {memcached_literal_param("\r\n")},
      {value, value_length},
      {memcached_literal_param("\r\n")}};

  /* Send command header */
  memcached_return_t rc = memcached_vdo(instance, vector, 12, flush);

  // If we should not reply, return with MEMCACHED_SUCCESS, unless error
  if (reply == false) {
    return memcached_success(rc) ? MEMCACHED_SUCCESS : rc;
  }

  if (flush == false) {
    return memcached_success(rc) ? MEMCACHED_BUFFERED : rc;
  }

  if (rc == MEMCACHED_SUCCESS) {
    char buffer[MEMCACHED_DEFAULT_COMMAND_SIZE];
    rc = memcached_response(instance, buffer, sizeof(buffer), NULL);

    if (rc == MEMCACHED_STORED) {
      return MEMCACHED_SUCCESS;
    }
  }

  assert(memcached_failed(rc));
#if 0
  if (memcached_has_error(ptr) == false)
  {
    return memcached_set_error(*ptr, rc, MEMCACHED_AT);
  }
#endif

  return rc;
}

static inline memcached_return_t
memcached_send(memcached_st *shell, const char *group_key, size_t group_key_length, const char *key,
               size_t key_length, const char *value, size_t value_length, const time_t expiration,
               const uint32_t flags, const uint64_t cas, memcached_storage_action_t verb) {
  Memcached *ptr = memcached2Memcached(shell);
  memcached_return_t rc;
  if (memcached_failed(rc = initialize_query(ptr, true))) {
    return rc;
  }

  if (memcached_failed(memcached_key_test(*ptr, (const char **) &key, &key_length, 1))) {
    return memcached_last_error(ptr);
  }

  uint32_t server_key =
      memcached_generate_hash_with_redistribution(ptr, group_key, group_key_length);
  memcached_instance_st *instance = memcached_instance_fetch(ptr, server_key);

  WATCHPOINT_SET(instance->io_wait_count.read = 0);
  WATCHPOINT_SET(instance->io_wait_count.write = 0);

  bool flush = true;
  if (memcached_is_buffering(instance->root) and verb == SET_OP) {
    flush = false;
  }

  bool reply = memcached_is_replying(ptr);

  hashkit_string_st *destination = NULL;

  if (memcached_is_encrypted(ptr)) {
    if (can_by_encrypted(verb) == false) {
      return memcached_set_error(
          *ptr, MEMCACHED_NOT_SUPPORTED, MEMCACHED_AT,
          memcached_literal_param("Operation not allowed while encyrption is enabled"));
    }

    if ((destination = hashkit_encrypt(&ptr->hashkit, value, value_length)) == NULL) {
      return rc;
    }
    value = hashkit_string_c_str(destination);
    value_length = hashkit_string_length(destination);
  }

  if (memcached_is_binary(ptr)) {
    rc = memcached_send_binary(ptr, instance, server_key, key, key_length, value, value_length,
                               expiration, flags, cas, flush, reply, verb);
  } else {
    rc = memcached_send_ascii(ptr, instance, key, key_length, value, value_length, expiration,
                              flags, cas, flush, reply, verb);
  }

  hashkit_string_free(destination);

  return rc;
}

memcached_return_t lrc_init(lrc_node *lrc, const char *key, size_t value_length, int32_t k, int32_t m, int32_t n_local, std::vector<int32_t> group, int32_t chunk_size) {
  if ((size_t)n_local != group.size() || k != accumulate(group.begin(), group.end(), 0) || n_local >= k) {
    return MEMCACHED_FAILURE;
  }

  lrc->k = k, lrc->m = m, lrc->n_local = n_local, lrc->chunk_size = chunk_size, lrc->n_chunk = k + m;
  lrc->key = new std::string(key);
  lrc->value_size = value_length;
  lrc->stripe_size = lrc->chunk_size * lrc->n_chunk;
  if (value_length % (size_t)lrc->stripe_size == 0) {
    lrc->n_stripe = value_length / (size_t)lrc->stripe_size;
  } else {
    lrc->n_stripe = value_length / (size_t)lrc->stripe_size + 1;
  }
  lrc->shape_group = (void *)(new std::vector<std::pair<int32_t, int32_t>>(n_local));
  std::vector<std::pair<int32_t, int32_t>> &sh_gr = *((std::vector<std::pair<int32_t, int32_t>> *)lrc->shape_group);
  int32_t pos = 0;
  for (int32_t i = 0; i < n_local; i++) {
    sh_gr[i].first = pos;
    sh_gr[i].second = group[i];
    pos += group[i];
  }

  int32_t *matrix_from_jerasure = reed_sol_vandermonde_coding_matrix(k, m - n_local, 8);
  lrc->encode_matrix = new int32_t[k * m];
  bzero(lrc->encode_matrix, k * m * sizeof(uint32_t));
  for (int32_t i = 0; i < n_local; i++) {
    for (int32_t j = 0; j < sh_gr[i].second; j++) {
      lrc->encode_matrix[i * k + sh_gr[i].first + j] = 1;
    }
  }
  for (int32_t i = 0; i < m - n_local; i++) {
    for (int32_t j = 0; j < k; j++) {
      lrc->encode_matrix[(i + n_local) * k + j] = matrix_from_jerasure[i * k + j];
    }
  }
  jerasure_print_matrix(lrc->encode_matrix, m, k, 8);

  lrc->erased = (void *)(new std::vector<bool>(k + m, false));
  std::vector<bool> &er = *((std::vector<bool> *)lrc->erased);
  // mark the code chunks as lost so that we can use decoder to encode.
  for (int32_t i = 0; i < m; i++) {
    er[k + i] = 1;
  }
  return MEMCACHED_SUCCESS;
}

memcached_return_t lrc_decoder_init(lrc_decoder *decoder, lrc_node *lrc, lrc_buf *buf, void *erased) {
  decoder->lrc = lrc;
  decoder->buf = *buf;
  decoder->erased = (void *)(new std::vector<bool>(lrc->k + lrc->m, false));
  decoder->needed = (void *)(new std::vector<bool>(lrc->k + lrc->m, false));
  std::vector<bool> &lrc_erased = *((std::vector<bool> *)lrc->erased);
  std::vector<bool> &decoder_erased = *((std::vector<bool> *)decoder->erased);
  std::vector<bool> &decoder_needed = *((std::vector<bool> *)decoder->needed);
  int32_t n_erased = 0;
  for (int32_t i = 0; i < lrc_erased.size(); i++) {
    if (lrc_erased[i] == true) {
      n_erased++;
    }
  }
  std::vector<std::pair<int32_t, int32_t>> &sh_gr = *((std::vector<std::pair<int32_t, int32_t>> *)lrc->shape_group);
  for (int32_t i = 0; i < lrc->n_local; i++) {
    int32_t erased_of_group = 0;
    int32_t begin = sh_gr[i].first;
    int32_t end = begin + sh_gr[i].second;
    for (int32_t j = begin; j < end; j++) {
      if (lrc_erased[j] == true) {
        erased_of_group++;
      }
    }
    if (lrc_erased[lrc->k + i] == true) {
      erased_of_group++;
    }
    if (erased_of_group == 0) {
      continue;
    }
    n_erased--;
    for (int32_t j = begin; j < end; j++) {
      decoder_needed[j] = (lrc_erased[j] == false);
    }
    decoder_needed[lrc->k + i] = (lrc_erased[lrc->k + i] == false);
  }
  if (n_erased > 0) {
    for (int32_t i = 0; i < lrc->k; i++) {
      decoder_needed[i] = (lrc_erased[i] == false);
    }
    for (int32_t i = lrc->k + lrc->n_local; i < lrc->k + lrc->m; i++) {
      decoder_needed[i] = (lrc_erased[i] == false);
      n_erased--;
      if (n_erased == 0) {
        break;
      }
    }
  }
  if (n_erased > 0) {
    return MEMCACHED_UNRECOVERABLE;
  }
  for (int32_t i = 0; i < lrc->k; i++) {
    decoder_erased[i] = lrc_erased[i];
  }
  decoder->decode_matrix = new int32_t[lrc->k * lrc->m];
  int32_t cur = 0;
  for (int32_t i = lrc->k; i < lrc->k + lrc->m; i++) {
    if (lrc_erased[i] == true || decoder_needed[i] == true) {
      decoder->buf.code_chunks[cur] = buf->code_chunks[i - lrc->k];
      decoder_erased[cur + lrc->k] = lrc_erased[i];
      memcpy(&decoder->decode_matrix[lrc->k * cur], &lrc->encode_matrix[lrc->k * (i - lrc->k)], lrc->k * sizeof(lrc->encode_matrix[0]));
      cur++;
    }
  }
  decoder->buf.n_code = cur;
  jerasure_print_matrix(decoder->decode_matrix, decoder->buf.n_code, lrc->k, 8);
  return MEMCACHED_SUCCESS;
}

int lrc_decoder_decode(lrc_decoder *decoder) {
  int cur = 0;
  int erasures[512] = {0};
  std::vector<bool> &decoder_erased = *((std::vector<bool> *)decoder->erased);
  for (int32_t i = 0; i < decoder->buf.n_data + decoder->buf.n_code; i++) {
    if (decoder_erased[i] == true) {
      erasures[cur] = i;
      cur++;
    }
  }
  erasures[cur] = -1;
  return jerasure_matrix_decode(decoder->buf.n_data, decoder->buf.n_code, 8, decoder->decode_matrix, 0, erasures, decoder->buf.data_chunks, decoder->buf.code_chunks, decoder->lrc->chunk_size);
}

void lrc_decoder_destroy(lrc_decoder *decoder) {
  delete[] decoder->decode_matrix;
  delete (std::vector<bool> *)decoder->erased;
  delete (std::vector<bool> *)decoder->needed;
}

memcached_return_t lrc_decode(lrc_node *lrc, lrc_buf *buf, void *erased) {
  memcached_return_t ret;
  lrc_decoder decoder;
  ret = lrc_decoder_init(&decoder, lrc, buf, erased);
  if (ret != MEMCACHED_SUCCESS) {
    return ret;
  }
  int ret_ = lrc_decoder_decode(&decoder);
  if (ret_ != 0) {
    ret = MEMCACHED_FAILURE;
  }
  lrc_decoder_destroy(&decoder);
  return ret;
}

memcached_return_t lrc_encode(lrc_node *lrc, lrc_buf *buf) {
  return lrc_decode(lrc, buf, lrc->erased);
}

void lrc_destroy_buf(lrc_buf *buf) {
  free(buf->stripe);
}

memcached_return_t memcached_set(memcached_st *ptr, const char *key, size_t key_length,
                                 const char *value, size_t value_length, time_t expiration,
                                 uint32_t flags) {
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_SET_START();

  std::unordered_map<std::string, lrc_node *> &lrc_helper = *((std::unordered_map<std::string, lrc_node *> *)ptr->lrc_helper);
  lrc_node *lrc = new lrc_node;
  if (lrc_init(lrc, key, value_length, 6, 4, 2, {3, 3}, 4) != MEMCACHED_SUCCESS) {
    return MEMCACHED_FAILURE;
  }
  lrc_helper.emplace(std::string(key), lrc);

  lrc_buf buf;
  buf.n_data = lrc->k;
  buf.n_code = lrc->m;
  buf.chunk_size = lrc->chunk_size;
  buf.aligned_chunk_size = (((buf.chunk_size - 1) / 16 + 1) * 16);
  posix_memalign((void **)&buf.stripe, 16, buf.aligned_chunk_size * lrc->n_chunk);
  for (int32_t i = 0; i < lrc->k; i++) {
    buf.data_chunks[i] = buf.stripe + buf.aligned_chunk_size * i;
  }
  for (int32_t i = 0; i < lrc->m; i++) {
    buf.code_chunks[i] = buf.data_chunks[lrc->k - 1] + buf.aligned_chunk_size * (i + 1);
  }

  std::vector<std::string> content_of_each_chunck(lrc->n_chunk);
  for (int32_t i = 0; i < lrc->n_stripe; i++) {
    bzero(buf.stripe, sizeof(char) * lrc->stripe_size);
    memcpy(buf.stripe, value + i * lrc->k * lrc->chunk_size, lrc->k * lrc->chunk_size);
    memcached_return_t ret = lrc_encode(lrc, &buf);
    if (ret != MEMCACHED_SUCCESS) {
      lrc_destroy_buf(&buf);
      return ret;
    }
    for (int32_t j = 0; j < lrc->n_chunk; j++) {
      if (j < lrc->k) {
        content_of_each_chunck[j].append(buf.data_chunks[j], lrc->chunk_size);
      } else {
        content_of_each_chunck[j].append(buf.code_chunks[j - lrc->k], lrc->chunk_size);
      }
    }
  }

  rc = memcached_send(ptr, key, key_length, key, key_length, value, value_length, expiration, flags,
                      0, SET_OP);
  lrc_destroy_buf(&buf);
  LIBMEMCACHED_MEMCACHED_SET_END();
  return rc;
}

memcached_return_t memcached_add(memcached_st *ptr, const char *key, size_t key_length,
                                 const char *value, size_t value_length, time_t expiration,
                                 uint32_t flags) {
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_ADD_START();
  rc = memcached_send(ptr, key, key_length, key, key_length, value, value_length, expiration, flags,
                      0, ADD_OP);

  LIBMEMCACHED_MEMCACHED_ADD_END();
  return rc;
}

memcached_return_t memcached_replace(memcached_st *ptr, const char *key, size_t key_length,
                                     const char *value, size_t value_length, time_t expiration,
                                     uint32_t flags) {
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_REPLACE_START();
  rc = memcached_send(ptr, key, key_length, key, key_length, value, value_length, expiration, flags,
                      0, REPLACE_OP);
  LIBMEMCACHED_MEMCACHED_REPLACE_END();
  return rc;
}

memcached_return_t memcached_prepend(memcached_st *ptr, const char *key, size_t key_length,
                                     const char *value, size_t value_length, time_t expiration,
                                     uint32_t flags) {
  memcached_return_t rc;
  rc = memcached_send(ptr, key, key_length, key, key_length, value, value_length, expiration, flags,
                      0, PREPEND_OP);
  return rc;
}

memcached_return_t memcached_append(memcached_st *ptr, const char *key, size_t key_length,
                                    const char *value, size_t value_length, time_t expiration,
                                    uint32_t flags) {
  memcached_return_t rc;
  rc = memcached_send(ptr, key, key_length, key, key_length, value, value_length, expiration, flags,
                      0, APPEND_OP);
  return rc;
}

memcached_return_t memcached_cas(memcached_st *ptr, const char *key, size_t key_length,
                                 const char *value, size_t value_length, time_t expiration,
                                 uint32_t flags, uint64_t cas) {
  memcached_return_t rc;
  rc = memcached_send(ptr, key, key_length, key, key_length, value, value_length, expiration, flags,
                      cas, CAS_OP);
  return rc;
}

memcached_return_t memcached_set_by_key(memcached_st *ptr, const char *group_key,
                                        size_t group_key_length, const char *key, size_t key_length,
                                        const char *value, size_t value_length, time_t expiration,
                                        uint32_t flags) {
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_SET_START();
  rc = memcached_send(ptr, group_key, group_key_length, key, key_length, value, value_length,
                      expiration, flags, 0, SET_OP);
  LIBMEMCACHED_MEMCACHED_SET_END();
  return rc;
}

memcached_return_t memcached_add_by_key(memcached_st *ptr, const char *group_key,
                                        size_t group_key_length, const char *key, size_t key_length,
                                        const char *value, size_t value_length, time_t expiration,
                                        uint32_t flags) {
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_ADD_START();
  rc = memcached_send(ptr, group_key, group_key_length, key, key_length, value, value_length,
                      expiration, flags, 0, ADD_OP);
  LIBMEMCACHED_MEMCACHED_ADD_END();
  return rc;
}

memcached_return_t memcached_replace_by_key(memcached_st *ptr, const char *group_key,
                                            size_t group_key_length, const char *key,
                                            size_t key_length, const char *value,
                                            size_t value_length, time_t expiration,
                                            uint32_t flags) {
  memcached_return_t rc;
  LIBMEMCACHED_MEMCACHED_REPLACE_START();
  rc = memcached_send(ptr, group_key, group_key_length, key, key_length, value, value_length,
                      expiration, flags, 0, REPLACE_OP);
  LIBMEMCACHED_MEMCACHED_REPLACE_END();
  return rc;
}

memcached_return_t memcached_prepend_by_key(memcached_st *ptr, const char *group_key,
                                            size_t group_key_length, const char *key,
                                            size_t key_length, const char *value,
                                            size_t value_length, time_t expiration,
                                            uint32_t flags) {
  return memcached_send(ptr, group_key, group_key_length, key, key_length, value, value_length,
                        expiration, flags, 0, PREPEND_OP);
}

memcached_return_t memcached_append_by_key(memcached_st *ptr, const char *group_key,
                                           size_t group_key_length, const char *key,
                                           size_t key_length, const char *value,
                                           size_t value_length, time_t expiration, uint32_t flags) {
  return memcached_send(ptr, group_key, group_key_length, key, key_length, value, value_length,
                        expiration, flags, 0, APPEND_OP);
}

memcached_return_t memcached_cas_by_key(memcached_st *ptr, const char *group_key,
                                        size_t group_key_length, const char *key, size_t key_length,
                                        const char *value, size_t value_length, time_t expiration,
                                        uint32_t flags, uint64_t cas) {
  return memcached_send(ptr, group_key, group_key_length, key, key_length, value, value_length,
                        expiration, flags, cas, CAS_OP);
}
