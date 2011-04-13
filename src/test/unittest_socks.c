#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "tinytest_macros.h"

#include <event2/buffer.h>

#define SOCKS_PRIVATE
#include "../socks.h"
#include "../plugins/obfs2_crypt.h"
#include "../util.h"
#include "../plugins/obfs2.h"

static void
test_socks_send_negotiation(void *data)
{
  struct evbuffer *dest = NULL;
  struct evbuffer *source = NULL;
  dest = evbuffer_new();
  source = evbuffer_new();

  socks_state_t *state;
  state = socks_state_new();
  tt_assert(state);

  /* First test:
     Only one method: NOAUTH.
     SOCKS proxy should like this. */

  uchar req1[2];
  req1[0] = 1;
  req1[1] = 0;

  evbuffer_add(source, req1, 2);

  tt_int_op(1, ==, socks5_handle_negotiation(source,dest,state));
  tt_int_op(0, ==, evbuffer_get_length(source));

  uchar rep1[2];
  tt_int_op(2, ==, evbuffer_remove(dest,rep1,2));
  tt_assert(rep1[0] == 5);
  tt_assert(rep1[1] == 0);

  /* Second test:
     Ten methods: One of them NOAUTH */
  uchar req2[10];
  req2[0] = 9;
  memset(req2+1,0x42,8);
  req2[9] = 0;

  evbuffer_add(source, req2, 10);

  tt_int_op(1, ==, socks5_handle_negotiation(source,dest,state));
  tt_int_op(0, ==, evbuffer_get_length(source));

  uchar rep2[2];
  tt_int_op(2, ==, evbuffer_remove(dest,rep2,2));
  tt_assert(rep2[0] == 5);
  tt_assert(rep2[1] == 0);

  /* Third test:
     100 methods: No NOAUTH */
  uchar req3[100];
  req3[0] = 99;
  memset(req3+1,0x42,99);

  evbuffer_add(source, req3, 100);

  tt_int_op(-1, ==, socks5_handle_negotiation(source,dest,state));
  tt_int_op(0, ==, evbuffer_get_length(source)); /* all data removed */

  uchar rep3[2];
  tt_int_op(2, ==, evbuffer_remove(dest,rep3,2));
  tt_assert(rep3[0] == 5);
  tt_assert(rep3[1] == 0xff);

  /* Fourth test:
     nmethods field = 4 but 3 actual methods.

     should say "0" for "want more data!"
   */
  uchar req4[4];
  req4[0] = 4;
  memset(req4+1,0x0,3);

  evbuffer_add(source, req4, 4);

  tt_int_op(0, ==, socks5_handle_negotiation(source,dest,state));
  tt_int_op(4, ==, evbuffer_get_length(source)); /* no bytes removed */
  evbuffer_drain(source, 4);

  /* Fifth test:
     nmethods field = 3 but 4 actual methods.
     Should be okay; the next byte is part of the request.
  */
  uchar req5[5];
  req5[0] = 3;
  memset(req5+1,0x0,4);

  evbuffer_add(source, req5, 5);

  tt_int_op(1, ==, socks5_handle_negotiation(source,dest,state));
  tt_int_op(1, ==, evbuffer_get_length(source)); /* 4 bytes removed */
  evbuffer_drain(source, 1);

  uchar rep4[2];
  tt_int_op(2, ==, evbuffer_remove(dest,rep4,2));
  tt_assert(rep4[0] == 5);
  tt_assert(rep4[1] == 0);

 end:
  if (state)
    socks_state_free(state);

  if (source)
    evbuffer_free(source);
  if (dest)
    evbuffer_free(dest);
}

static void
test_socks_socks5_request(void *data)
{

  struct evbuffer *dest = NULL;
  struct evbuffer *source = NULL;
  dest = evbuffer_new();
  source = evbuffer_new();

  socks_state_t *state;
  state = socks_state_new();
  tt_assert(state);

  const uint32_t addr = htonl(0x7f000001); /* 127.0.0.1 */
  const uint16_t port = htons(80);    /* 80 */

  /* First test:
     Broken IPV4 req packet with no destport */
  struct parsereq pr1;
  uchar req1[7];
  req1[0] = 1;
  req1[1] = 0;
  req1[2] = 1;
  memcpy(req1+3,&addr,4);

  evbuffer_add(source, "\x05", 1);
  evbuffer_add(source, req1, 7);
  tt_int_op(0, ==, socks5_handle_request(source,&pr1)); /* 0: want more data*/

  /* emptying source buffer before next test  */
  size_t buffer_len = evbuffer_get_length(source);
  tt_int_op(0, ==, evbuffer_drain(source, buffer_len));

  /* Second test:
     Broken FQDN req packet with no destport */
  uchar req2[7];
  req2[0] = 1;
  req2[1] = 0;
  req2[2] = 3;
  req2[3] = 15;
  memcpy(req1+4,&addr,3);

  evbuffer_add(source, "\x05", 1);
  evbuffer_add(source, req2, 7);
  tt_int_op(0, ==, socks5_handle_request(source,&pr1)); /* 0: want more data*/

  /* emptying source buffer before next test  */
  buffer_len = evbuffer_get_length(source);
  tt_int_op(0, ==, evbuffer_drain(source, buffer_len));

  /* Third test:
     Correct IPV4 req packet. */
  uchar req3[9];
  req3[0] = 1;
  req3[1] = 0;
  req3[2] = 1;
  memcpy(req3+3,&addr,4);
  memcpy(req3+7,&port,2);

  evbuffer_add(source, "\x05", 1);
  evbuffer_add(source, req3, 9);
  tt_int_op(1, ==, socks5_handle_request(source,&pr1));
  tt_str_op(pr1.addr, ==, "127.0.0.1");
  tt_int_op(pr1.port, ==, 80);

  /* emptying source buffer before next test  */
  buffer_len = evbuffer_get_length(source);
  tt_int_op(0, ==, evbuffer_drain(source, buffer_len));

  /* Fourth test:
     Correct FQDN req packet. */
  const char fqdn[17] = "www.test.example";
  uchar req4[24];
  req4[0] = 5;
  req4[1] = 1;
  req4[2] = 0;
  req4[3] = 3;
  req4[4] = strlen(fqdn);
  strcpy((char *)req4+5,fqdn);
  memcpy(req4+5+strlen(fqdn),&port,2);

  evbuffer_add(source, req4, 24);
  tt_int_op(1, ==, socks5_handle_request(source,&pr1));
  tt_assert(strcmp(pr1.addr, "www.test.example") == 0);
  tt_int_op(pr1.port, ==, 80);

  /* Third test:
     Small request packet */
  uchar req5[3];
  req5[0] = 5;
  req5[1] = 1;
  req5[2] = 0;

  evbuffer_add(source,req5,3);
  tt_int_op(0, ==, socks5_handle_request(source,&pr1));

  /* emptying source buffer before next test  */
  buffer_len = evbuffer_get_length(source);
  tt_int_op(0, ==, evbuffer_drain(source, buffer_len));

  /* Fourth test:
     Wrong Reserved field */
  uchar req6[5];
  req6[0] = 5;
  req6[1] = 1;
  req6[2] = 1;
  req6[3] = 42;
  req6[4] = 42;

  evbuffer_add(source,req6,5);
  tt_int_op(-1, ==, socks5_handle_request(source,&pr1));

  /* emptying source buffer before next test  */
  buffer_len = evbuffer_get_length(source);
  tt_int_op(0, ==, evbuffer_drain(source, buffer_len));

 end:
  if (state)
    socks_state_free(state);

  if (source)
    evbuffer_free(source);
  if (dest)
    evbuffer_free(dest);
}

static void
test_socks_socks5_request_reply(void *data)
{
  struct evbuffer *reply_dest = NULL;
  reply_dest = evbuffer_new();

  socks_state_t *state;
  state = socks_state_new();
  tt_assert(state);

  state->parsereq.af = AF_INET;
  strcpy(state->parsereq.addr, "127.0.0.1");
  state->parsereq.port = 7357;

  tt_int_op(1, ==, socks5_send_reply(reply_dest,
                                     state, SOCKS5_REP_SUCCESS));

  uchar rep1[255];
  evbuffer_remove(reply_dest,rep1,255); /* yes, this is dirty */

  tt_assert(rep1[3] == SOCKS5_ATYP_IPV4);
  /* check address */
  tt_int_op(0, ==, memcmp(rep1+4, "\x7f\x00\x00\x01", 4));
  /* check port */
  tt_int_op(0, ==, memcmp(rep1+4+4,"\x1c\xbd",2));

  /* emptying reply_dest buffer before next test  */
  size_t buffer_len = evbuffer_get_length(reply_dest);
  tt_int_op(0, ==, evbuffer_drain(reply_dest, buffer_len));

  const char *fqdn = "www.test.example";
  state->parsereq.af = AF_UNSPEC;
  strcpy(state->parsereq.addr, fqdn);
  state->parsereq.port = 7357;

  tt_int_op(-1, ==, socks5_send_reply(reply_dest,
                                     state, SOCKS5_REP_FAIL));

  uchar rep2[255];
  evbuffer_remove(reply_dest,rep2,255);

  tt_assert(rep2[3] == SOCKS5_ATYP_FQDN);
  tt_assert(rep2[4] == strlen(fqdn));
  /* check fqdn */
  tt_int_op(0, ==, memcmp(rep2+5,fqdn,strlen(fqdn)));
  /* check port */
  tt_int_op(0, ==, memcmp(rep2+5+strlen(fqdn),"\x1c\xbd",2));

 end:
  if (state)
    socks_state_free(state);

  if (reply_dest)
    evbuffer_free(reply_dest);
}


#define T(name, flags) \
  { #name, test_socks_##name, (flags), NULL, NULL }

struct testcase_t socks_tests[] = {
  T(send_negotiation, 0),
  T(socks5_request, 0),
  T(socks5_request_reply, 0),
  END_OF_TESTCASES
};