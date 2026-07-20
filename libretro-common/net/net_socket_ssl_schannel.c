/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* Desktop Windows TLS backend. SChannel uses the Windows trust store and
 * performs certificate-chain and host-name validation automatically. */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <net/net_compat.h>
#include <net/net_socket.h>
#include <net/net_socket_ssl.h>

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <security.h>
#include <schannel.h>

#define SCHANNEL_READ_CHUNK       16384
#define SCHANNEL_HANDSHAKE_TIMEOUT 5000
#define SCHANNEL_CLOSE_TIMEOUT       250
#define SCHANNEL_MAX_HANDSHAKE_BYTES (256 * 1024)
#define SCHANNEL_MAX_READ_STEPS       8

struct ssl_state
{
   int fd;
   char *domain;
   CredHandle credentials;
   CtxtHandle context;
   SecPkgContext_StreamSizes stream_sizes;
   unsigned long request_flags;
   uint8_t *encrypted;
   size_t encrypted_len;
   size_t encrypted_capacity;
   uint8_t *plain;
   size_t plain_offset;
   size_t plain_len;
   uint8_t *send_buffer;
   size_t send_capacity;
   size_t send_offset;
   size_t send_len;
   bool have_credentials;
   bool have_context;
   bool handshake_complete;
   bool socket_closed;
   bool transport_closed;
   bool failed;
};

static bool schannel_reserve(uint8_t **buffer,
      size_t *capacity, size_t required)
{
   uint8_t *new_buffer;
   size_t new_capacity = *capacity ? *capacity : SCHANNEL_READ_CHUNK;

   if (required <= *capacity)
      return true;

   while (new_capacity < required)
   {
      size_t previous = new_capacity;
      new_capacity *= 2;
      if (new_capacity < previous)
         return false;
   }

   new_buffer = (uint8_t*)realloc(*buffer, new_capacity);
   if (!new_buffer)
      return false;

   *buffer   = new_buffer;
   *capacity = new_capacity;
   return true;
}

static bool schannel_preserve_extra(struct ssl_state *state,
      const SecBuffer *buffers, size_t buffer_count)
{
   size_t i;
   const SecBuffer *extra = NULL;
   const uint8_t *source;

   for (i = 0; i < buffer_count; i++)
      if (buffers[i].BufferType == SECBUFFER_EXTRA)
      {
         extra = &buffers[i];
         break;
      }

   if (!extra)
   {
      state->encrypted_len = 0;
      return true;
   }

   if (extra->cbBuffer > state->encrypted_len)
      return false;

   source = state->encrypted + state->encrypted_len - extra->cbBuffer;
   if (extra->pvBuffer &&
       (uintptr_t)extra->pvBuffer >= (uintptr_t)state->encrypted &&
       (uintptr_t)extra->pvBuffer - (uintptr_t)state->encrypted <=
          state->encrypted_len - extra->cbBuffer)
      source = (const uint8_t*)extra->pvBuffer;

   memmove(state->encrypted, source, extra->cbBuffer);
   state->encrypted_len = extra->cbBuffer;
   return true;
}

static bool schannel_prepare_renegotiation(struct ssl_state *state,
      const SecBuffer *token)
{
   const uint8_t *source;

   if (!token || !token->cbBuffer || token->cbBuffer > state->encrypted_len)
      return false;

   source = state->encrypted + state->encrypted_len - token->cbBuffer;
   if (token->pvBuffer &&
       (uintptr_t)token->pvBuffer >= (uintptr_t)state->encrypted &&
       (uintptr_t)token->pvBuffer - (uintptr_t)state->encrypted <=
          state->encrypted_len - token->cbBuffer)
      source = (const uint8_t*)token->pvBuffer;

   memmove(state->encrypted, source, token->cbBuffer);
   state->encrypted_len = token->cbBuffer;
   return true;
}

static int schannel_time_remaining(ULONGLONG deadline)
{
   ULONGLONG now = GetTickCount64();
   ULONGLONG remaining;

   if (now >= deadline)
      return 0;

   remaining = deadline - now;
   return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static ssize_t schannel_read_encrypted(struct ssl_state *state,
      bool blocking, size_t maximum_total, int timeout)
{
   int ret;
   size_t available;
   size_t target;

   if (state->encrypted_len > SIZE_MAX - SCHANNEL_READ_CHUNK)
      return -1;

   target = state->encrypted_len + SCHANNEL_READ_CHUNK;
   if (maximum_total && target > maximum_total)
      target = maximum_total;
   if (target <= state->encrypted_len)
      return -1;

   if (!schannel_reserve(&state->encrypted, &state->encrypted_capacity,
            target))
      return -1;

   if (blocking)
   {
      bool readable = true;
      if (timeout <= 0 ||
          !socket_wait(state->fd, &readable, NULL, timeout) || !readable)
         return -1;
   }

   available = target - state->encrypted_len;
   if (available > INT_MAX)
      available = INT_MAX;

   ret = recv(state->fd, (char*)state->encrypted + state->encrypted_len,
         (int)available, 0);
   if (ret > 0)
   {
      state->encrypted_len += (size_t)ret;
      return ret;
   }

   if (ret < 0 && isagain(ret))
      return blocking ? -1 : 0;

   state->transport_closed = true;
   return -1;
}

static void schannel_free_output_buffers(SecBuffer *outputs,
      size_t output_count)
{
   size_t i;

   for (i = 0; i < output_count; i++)
      if (outputs[i].pvBuffer)
      {
         FreeContextBuffer(outputs[i].pvBuffer);
         outputs[i].pvBuffer = NULL;
         outputs[i].cbBuffer = 0;
      }
}

static bool schannel_send_output_buffers(struct ssl_state *state,
      SecBuffer *outputs, size_t output_count, int timeout)
{
   bool success = true;
   size_t i;

   for (i = 0; i < output_count; i++)
      if (outputs[i].pvBuffer && outputs[i].cbBuffer && success &&
          outputs[i].BufferType == SECBUFFER_TOKEN)
         success = socket_send_all_blocking_with_timeout(state->fd,
               outputs[i].pvBuffer, outputs[i].cbBuffer, timeout, true);

   schannel_free_output_buffers(outputs, output_count);

   return success;
}

static bool schannel_handshake(struct ssl_state *state, bool initial)
{
   SECURITY_STATUS status = SEC_I_CONTINUE_NEEDED;
   TimeStamp expiry;
   bool first = initial;
   unsigned long final_context_attrs = 0;
   ULONGLONG deadline = GetTickCount64() + SCHANNEL_HANDSHAKE_TIMEOUT;

   /* The loop is synchronous, but the socket stays nonblocking so both reads
    * and writes can enforce the single handshake deadline with socket_wait. */
   if (!socket_set_block(state->fd, false))
      return false;

   while (status != SEC_E_OK)
   {
      SecBuffer input_buffers[2];
      SecBuffer output_buffers[3];
      SecBufferDesc input_desc;
      SecBufferDesc output_desc;
      unsigned long context_attrs = 0;
      int remaining;

      if (!first && !state->encrypted_len)
      {
         remaining = schannel_time_remaining(deadline);
         if (schannel_read_encrypted(state, true,
                  SCHANNEL_MAX_HANDSHAKE_BYTES, remaining) <= 0)
            return false;
      }

      memset(input_buffers, 0, sizeof(input_buffers));
      memset(output_buffers, 0, sizeof(output_buffers));
      memset(&input_desc, 0, sizeof(input_desc));
      memset(&output_desc, 0, sizeof(output_desc));

      output_buffers[0].BufferType = SECBUFFER_TOKEN;
      output_buffers[1].BufferType = SECBUFFER_ALERT;
      output_buffers[2].BufferType = SECBUFFER_EMPTY;
      output_desc.ulVersion        = SECBUFFER_VERSION;
      output_desc.cBuffers         = 3;
      output_desc.pBuffers         = output_buffers;

      if (!first)
      {
         input_buffers[0].BufferType = SECBUFFER_TOKEN;
         input_buffers[0].pvBuffer   = state->encrypted;
         input_buffers[0].cbBuffer   = (unsigned long)state->encrypted_len;
         input_buffers[1].BufferType = SECBUFFER_EMPTY;
         input_desc.ulVersion        = SECBUFFER_VERSION;
         input_desc.cBuffers         = 2;
         input_desc.pBuffers         = input_buffers;
      }

      status = InitializeSecurityContextA(
            &state->credentials,
            first ? NULL : &state->context,
            (SEC_CHAR*)state->domain,
            state->request_flags,
            0,
            SECURITY_NATIVE_DREP,
            first ? NULL : &input_desc,
            0,
            &state->context,
            &output_desc,
            &context_attrs,
            &expiry);

      final_context_attrs = context_attrs;

      /* COMPLETE_* also returns a live context. Record ownership before
       * CompleteAuthToken so a completion failure cannot leak the handle. */
      if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED ||
          status == SEC_I_COMPLETE_NEEDED ||
          status == SEC_I_COMPLETE_AND_CONTINUE)
         state->have_context = true;

      if (status == SEC_E_INCOMPLETE_MESSAGE)
      {
         schannel_free_output_buffers(output_buffers, 3);
         first = false;
         remaining = schannel_time_remaining(deadline);
         if (schannel_read_encrypted(state, true,
                  SCHANNEL_MAX_HANDSHAKE_BYTES, remaining) <= 0)
            return false;
         continue;
      }

      if (status == SEC_I_COMPLETE_NEEDED ||
          status == SEC_I_COMPLETE_AND_CONTINUE)
      {
         SECURITY_STATUS complete_status = CompleteAuthToken(
               &state->context, &output_desc);
         if (complete_status != SEC_E_OK)
         {
            schannel_free_output_buffers(output_buffers, 3);
            return false;
         }

         status = status == SEC_I_COMPLETE_NEEDED
               ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
      }

      remaining = schannel_time_remaining(deadline);
      if (remaining <= 0)
      {
         schannel_free_output_buffers(output_buffers, 3);
         return false;
      }
      if (!schannel_send_output_buffers(state,
               output_buffers, 3, remaining))
         return false;

      if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
         return false;

      if (!first && !schannel_preserve_extra(state,
               input_buffers, 2))
         return false;

      first = false;
   }

   if ((final_context_attrs & (ISC_RET_CONFIDENTIALITY | ISC_RET_STREAM)) !=
       (ISC_RET_CONFIDENTIALITY | ISC_RET_STREAM))
      return false;

   if (QueryContextAttributes(&state->context,
            SECPKG_ATTR_STREAM_SIZES, &state->stream_sizes) != SEC_E_OK)
      return false;

   state->handshake_complete = true;
   return true;
}

static bool schannel_store_plain(struct ssl_state *state,
      const uint8_t *data, size_t len)
{
   uint8_t *new_buffer;

   if (!len)
      return true;

   new_buffer = (uint8_t*)realloc(state->plain, len);
   if (!new_buffer)
      return false;

   state->plain        = new_buffer;
   state->plain_offset = 0;
   state->plain_len    = len;
   memcpy(state->plain, data, len);
   return true;
}

static size_t schannel_copy_pending_plain(struct ssl_state *state,
      uint8_t *data, size_t len)
{
   size_t available;
   size_t copy_len;

   if (state->plain_offset >= state->plain_len)
      return 0;

   available = state->plain_len - state->plain_offset;
   copy_len  = available < len ? available : len;
   memcpy(data, state->plain + state->plain_offset, copy_len);
   state->plain_offset += copy_len;

   if (state->plain_offset == state->plain_len)
   {
      state->plain_offset = 0;
      state->plain_len    = 0;
   }

   return copy_len;
}

/* Nonblocking writes may consume an application buffer after placing the
 * encrypted record in this bounded state buffer. Always finish that record
 * before waiting for a response, otherwise the peer can wait forever for the
 * tail of the request while the caller waits for the peer's response. */
static int schannel_flush_pending_send(struct ssl_state *state,
      bool blocking, bool no_signal)
{
   ssize_t sent;

   if (state->send_offset >= state->send_len)
   {
      state->send_offset = 0;
      state->send_len    = 0;
      return 1;
   }

   if (blocking)
   {
      if (!socket_send_all_blocking(state->fd,
               state->send_buffer + state->send_offset,
               state->send_len - state->send_offset, no_signal))
      {
         state->failed = true;
         return -1;
      }

      state->send_offset = 0;
      state->send_len    = 0;
      return 1;
   }

   sent = socket_send_all_nonblocking(state->fd,
         state->send_buffer + state->send_offset,
         state->send_len - state->send_offset, no_signal);
   if (sent < 0)
   {
      state->failed = true;
      return -1;
   }

   state->send_offset += (size_t)sent;
   if (state->send_offset < state->send_len)
      return 0;

   state->send_offset = 0;
   state->send_len    = 0;
   return 1;
}

static ssize_t schannel_receive(struct ssl_state *state, bool *error,
      void *data_, size_t len, bool blocking)
{
   uint8_t *data = (uint8_t*)data_;
   size_t total  = 0;
   int flush_status;
   bool need_more_ciphertext = false;
   int steps = blocking ? INT_MAX : SCHANNEL_MAX_READ_STEPS;

   if (!len)
      return 0;

   total = schannel_copy_pending_plain(state, data, len);
   if (total == len)
      return (ssize_t)total;

   if (state->failed || state->transport_closed)
   {
      if (total)
         return (ssize_t)total;
      *error = true;
      return -1;
   }

   if (!socket_set_block(state->fd, blocking))
   {
      state->failed = true;
      if (total)
         return (ssize_t)total;
      *error = true;
      return -1;
   }

   flush_status = schannel_flush_pending_send(state, blocking, true);
   if (flush_status < 0)
   {
      if (total)
         return (ssize_t)total;
      *error = true;
      return -1;
   }
   if (!flush_status)
      return (ssize_t)total;

   while (total < len && steps-- > 0)
   {
      SECURITY_STATUS status;
      SecBuffer buffers[4];
      SecBufferDesc desc;
      SecBuffer *plain_buffer = NULL;
      const SecBuffer *renegotiation_token = NULL;
      size_t i;

      if (!state->encrypted_len || need_more_ciphertext)
      {
         ssize_t read_len = schannel_read_encrypted(state, blocking, 0,
               blocking ? SCHANNEL_HANDSHAKE_TIMEOUT : 0);
         if (read_len <= 0)
         {
            if (read_len < 0)
            {
               if (!state->transport_closed)
                  state->failed = true;
               if (!total)
               {
                  *error = true;
                  return -1;
               }
            }
            break;
         }
         need_more_ciphertext = false;
      }

      memset(buffers, 0, sizeof(buffers));
      buffers[0].BufferType = SECBUFFER_DATA;
      buffers[0].pvBuffer   = state->encrypted;
      buffers[0].cbBuffer   = (unsigned long)state->encrypted_len;
      buffers[1].BufferType = SECBUFFER_EMPTY;
      buffers[2].BufferType = SECBUFFER_EMPTY;
      buffers[3].BufferType = SECBUFFER_EMPTY;
      desc.ulVersion        = SECBUFFER_VERSION;
      desc.cBuffers         = 4;
      desc.pBuffers         = buffers;

      status = DecryptMessage(&state->context, &desc, 0, NULL);

      if (status == SEC_E_INCOMPLETE_MESSAGE)
      {
         need_more_ciphertext = true;
         continue;
      }

      if (status != SEC_E_OK &&
          status != SEC_I_RENEGOTIATE &&
          status != SEC_I_CONTEXT_EXPIRED)
      {
         state->failed = true;
         if (!total)
         {
            *error = true;
            return -1;
         }
         break;
      }

      if (status == SEC_I_RENEGOTIATE)
      {
         /* EXTRA is the token when present. Otherwise Microsoft requires
          * reusing the same input buffer modified by DecryptMessage. */
         for (i = 0; i < 4; i++)
            if (buffers[i].BufferType == SECBUFFER_EXTRA)
            {
               renegotiation_token = &buffers[i];
               break;
            }
         if (!renegotiation_token)
            renegotiation_token = &buffers[0];
      }

      /* On renegotiation or close, buffers[0] may still be the encrypted
       * protocol token and retain SECBUFFER_DATA. Never expose it as HTTP
       * plaintext. Valid application data, when supplied, is in buffers 1-3. */
      for (i = status == SEC_E_OK ? 0 : 1; i < 4; i++)
         if (buffers[i].BufferType == SECBUFFER_DATA)
         {
            if (&buffers[i] == renegotiation_token)
               continue;
            plain_buffer = &buffers[i];
            break;
         }

      if (plain_buffer && plain_buffer->cbBuffer)
      {
         size_t available = len - total;
         size_t copy_len  = plain_buffer->cbBuffer < available
               ? plain_buffer->cbBuffer : available;
         const uint8_t *plain_data = (const uint8_t*)plain_buffer->pvBuffer;

         memcpy(data + total, plain_data, copy_len);
         total += copy_len;

         if (copy_len < plain_buffer->cbBuffer &&
             !schannel_store_plain(state, plain_data + copy_len,
                plain_buffer->cbBuffer - copy_len))
         {
            state->failed = true;
            break;
         }
      }

      if (status == SEC_I_CONTEXT_EXPIRED)
      {
         state->transport_closed = true;
         break;
      }

      if (status == SEC_I_RENEGOTIATE)
      {
         if (!schannel_prepare_renegotiation(state,
                  renegotiation_token) ||
             !schannel_handshake(state, false) ||
             !socket_set_block(state->fd, blocking))
         {
            state->failed = true;
            if (!total)
            {
               *error = true;
               return -1;
            }
            break;
         }
         continue;
      }

      if (!schannel_preserve_extra(state, buffers, 4))
      {
         state->failed = true;
         break;
      }
   }

   if (!total && (state->failed || state->transport_closed))
   {
      *error = true;
      return -1;
   }

   return (ssize_t)total;
}

void *ssl_socket_init(int fd, const char *domain)
{
   SCHANNEL_CRED credential_data;
   SECURITY_STATUS status;
   TimeStamp expiry;
   struct ssl_state *state = (struct ssl_state*)calloc(1, sizeof(*state));

   if (!state)
      return NULL;

   state->fd     = fd;
   state->domain = strdup(domain);
   if (!state->domain)
      goto error;

   memset(&credential_data, 0, sizeof(credential_data));
   credential_data.dwVersion = SCHANNEL_CRED_VERSION;
   credential_data.dwFlags   = SCH_CRED_AUTO_CRED_VALIDATION |
                               SCH_CRED_NO_DEFAULT_CREDS;
#ifdef SCH_USE_STRONG_CRYPTO
   credential_data.dwFlags  |= SCH_USE_STRONG_CRYPTO;
#endif

   status = AcquireCredentialsHandleA(NULL, UNISP_NAME_A,
         SECPKG_CRED_OUTBOUND, NULL, &credential_data, NULL, NULL,
         &state->credentials, &expiry);
   if (status != SEC_E_OK)
      goto error;

   state->have_credentials = true;
   state->request_flags    = ISC_REQ_SEQUENCE_DETECT |
                             ISC_REQ_REPLAY_DETECT |
                             ISC_REQ_CONFIDENTIALITY |
                             ISC_REQ_EXTENDED_ERROR |
                             ISC_REQ_ALLOCATE_MEMORY |
                             ISC_REQ_STREAM;
   return state;

error:
   free(state->domain);
   free(state);
   return NULL;
}

int ssl_socket_connect(void *state_data,
      void *data, bool timeout_enable, bool nonblock)
{
   struct ssl_state *state = (struct ssl_state*)state_data;

   if (!state)
      return -1;

   if (timeout_enable)
   {
      if (!socket_connect_with_timeout(state->fd, data, 5000))
         return -1;
      if (!socket_set_block(state->fd, true))
         return -1;
   }
   else if (socket_connect(state->fd, data))
      return -1;

   if (!schannel_handshake(state, true))
      return -1;

   if (!socket_set_block(state->fd, !nonblock))
      return -1;

   return state->fd;
}

static bool schannel_encrypt_record(struct ssl_state *state,
      const uint8_t *data, size_t len)
{
   SECURITY_STATUS status;
   SecBuffer buffers[4];
   SecBufferDesc desc;
   size_t maximum_message = state->stream_sizes.cbMaximumMessage;
   size_t required;

   if (!maximum_message || len > maximum_message)
      return false;

   required = state->stream_sizes.cbHeader + maximum_message;
   if (required < maximum_message)
      return false;
   required += state->stream_sizes.cbTrailer;
   if (required < maximum_message)
      return false;

   if (!schannel_reserve(&state->send_buffer,
            &state->send_capacity, required))
      return false;

   memcpy(state->send_buffer + state->stream_sizes.cbHeader, data, len);

   memset(buffers, 0, sizeof(buffers));
   buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
   buffers[0].pvBuffer   = state->send_buffer;
   buffers[0].cbBuffer   = state->stream_sizes.cbHeader;
   buffers[1].BufferType = SECBUFFER_DATA;
   buffers[1].pvBuffer   = state->send_buffer + state->stream_sizes.cbHeader;
   buffers[1].cbBuffer   = (unsigned long)len;
   buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
   buffers[2].pvBuffer   = state->send_buffer +
                           state->stream_sizes.cbHeader + len;
   buffers[2].cbBuffer   = state->stream_sizes.cbTrailer;
   buffers[3].BufferType = SECBUFFER_EMPTY;
   desc.ulVersion        = SECBUFFER_VERSION;
   desc.cBuffers         = 4;
   desc.pBuffers         = buffers;

   status = EncryptMessage(&state->context, 0, &desc, 0);
   if (status != SEC_E_OK)
      return false;

   state->send_offset = 0;
   state->send_len    = (size_t)buffers[0].cbBuffer +
                        (size_t)buffers[1].cbBuffer +
                        (size_t)buffers[2].cbBuffer;
   return true;
}

int ssl_socket_send_all_blocking(void *state_data,
      const void *data_, size_t len, bool no_signal)
{
   struct ssl_state *state = (struct ssl_state*)state_data;
   const uint8_t *data = (const uint8_t*)data_;
   size_t maximum_message;

   if (!state || !state->handshake_complete || state->failed ||
       state->transport_closed)
      return false;

   if (!socket_set_block(state->fd, true))
      return false;

   if (schannel_flush_pending_send(state, true, no_signal) < 0)
      return false;

   maximum_message = state->stream_sizes.cbMaximumMessage;
   if (!maximum_message)
      return false;

   while (len)
   {
      size_t chunk = len < maximum_message ? len : maximum_message;

      if (!schannel_encrypt_record(state, data, chunk))
      {
         state->failed = true;
         return false;
      }

      if (!socket_send_all_blocking(state->fd,
               state->send_buffer, state->send_len, no_signal))
      {
         state->failed = true;
         return false;
      }

      state->send_offset = 0;
      state->send_len    = 0;
      data += chunk;
      len  -= chunk;
   }

   return true;
}

ssize_t ssl_socket_send_all_nonblocking(void *state_data,
      const void *data_, size_t len, bool no_signal)
{
   struct ssl_state *state = (struct ssl_state*)state_data;
   size_t chunk;
   ssize_t sent;

   if (!state || !state->handshake_complete || state->failed ||
       state->transport_closed)
      return -1;

   if (!socket_set_block(state->fd, false))
      return -1;

   if (schannel_flush_pending_send(state, false, no_signal) < 1)
      return state->failed ? -1 : 0;

   if (!len)
      return 0;

   chunk = len < state->stream_sizes.cbMaximumMessage
         ? len : state->stream_sizes.cbMaximumMessage;
   if (!chunk || !schannel_encrypt_record(state,
            (const uint8_t*)data_, chunk))
   {
      state->failed = true;
      return -1;
   }

   sent = socket_send_all_nonblocking(state->fd,
         state->send_buffer, state->send_len, no_signal);
   if (sent < 0)
   {
      state->failed = true;
      return -1;
   }

   state->send_offset = (size_t)sent;
   if (state->send_offset == state->send_len)
   {
      state->send_offset = 0;
      state->send_len    = 0;
   }

   /* Plaintext is consumed once it is encrypted into our bounded record
    * buffer, even if the socket will flush the ciphertext on a later call. */
   return (ssize_t)chunk;
}

int ssl_socket_receive_all_blocking(void *state_data,
      void *data_, size_t len)
{
   bool error = false;
   ssize_t received = schannel_receive((struct ssl_state*)state_data,
         &error, data_, len, true);
   return !error && received == (ssize_t)len;
}

ssize_t ssl_socket_receive_all_nonblocking(void *state_data,
      bool *error, void *data_, size_t len)
{
   return schannel_receive((struct ssl_state*)state_data,
         error, data_, len, false);
}

void ssl_socket_close(void *state_data)
{
   struct ssl_state *state = (struct ssl_state*)state_data;

   if (!state || state->socket_closed)
      return;

   if (state->have_context && state->handshake_complete && !state->failed)
   {
      DWORD shutdown_token = SCHANNEL_SHUTDOWN;
      SecBuffer input_buffer;
      SecBuffer output_buffers[3];
      SecBufferDesc input_desc;
      SecBufferDesc output_desc;
      unsigned long context_attrs = 0;
      TimeStamp expiry;

      /* Timeout-aware send helpers require a nonblocking socket; otherwise
       * send() itself may wait forever before they can enforce the deadline. */
      socket_set_block(state->fd, false);

      if (state->send_offset < state->send_len)
      {
         socket_send_all_blocking_with_timeout(state->fd,
               state->send_buffer + state->send_offset,
               state->send_len - state->send_offset,
               SCHANNEL_CLOSE_TIMEOUT, true);
         state->send_offset = 0;
         state->send_len    = 0;
      }

      memset(&input_buffer, 0, sizeof(input_buffer));
      input_buffer.BufferType = SECBUFFER_TOKEN;
      input_buffer.pvBuffer   = &shutdown_token;
      input_buffer.cbBuffer   = sizeof(shutdown_token);
      input_desc.ulVersion    = SECBUFFER_VERSION;
      input_desc.cBuffers     = 1;
      input_desc.pBuffers     = &input_buffer;

      if (ApplyControlToken(&state->context, &input_desc) == SEC_E_OK)
      {
         SECURITY_STATUS status;
         memset(output_buffers, 0, sizeof(output_buffers));
         output_buffers[0].BufferType = SECBUFFER_TOKEN;
         output_buffers[1].BufferType = SECBUFFER_ALERT;
         output_buffers[2].BufferType = SECBUFFER_EMPTY;
         output_desc.ulVersion        = SECBUFFER_VERSION;
         output_desc.cBuffers         = 3;
         output_desc.pBuffers         = output_buffers;

         status = InitializeSecurityContextA(&state->credentials,
               &state->context, (SEC_CHAR*)state->domain,
               state->request_flags, 0, SECURITY_NATIVE_DREP,
               NULL, 0, &state->context, &output_desc,
               &context_attrs, &expiry);
         if (status == SEC_E_OK || status == SEC_I_CONTEXT_EXPIRED ||
             status == SEC_I_CONTINUE_NEEDED)
            schannel_send_output_buffers(state,
                  output_buffers, 3, SCHANNEL_CLOSE_TIMEOUT);
         else
            schannel_free_output_buffers(output_buffers, 3);
      }
   }

   socket_close(state->fd);
   state->socket_closed = true;
}

void ssl_socket_free(void *state_data)
{
   struct ssl_state *state = (struct ssl_state*)state_data;

   if (!state)
      return;

   if (state->have_context)
      DeleteSecurityContext(&state->context);
   if (state->have_credentials)
      FreeCredentialsHandle(&state->credentials);

   free(state->send_buffer);
   free(state->plain);
   free(state->encrypted);
   free(state->domain);
   free(state);
}
