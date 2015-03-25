/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2015, Digium, Inc.
 *
 * Joshua Colp <jcolp@digium.com>
 * Mark Michelson <mmichelson@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*** MODULEINFO
	<depend>TEST_FRAMEWORK</depend>
	<support_level>core</support_level>
 ***/

#include "asterisk.h"

#include <arpa/nameser.h>

#include "asterisk/test.h"
#include "asterisk/module.h"
#include "asterisk/dns_core.h"
#include "asterisk/dns_resolver.h"
#include "asterisk/dns_srv.h"

#define DNS_HEADER_SIZE 96

const char DNS_HEADER[] = {
	/* ID  == 0 */
	0x00, 0x00,
	/* QR == 1, Opcode == 0, AA == 1, TC == 0, RD == 1 */
	0x85,
	/* RA == 1, Z == 0, RCODE == 0 */
	0x80,
	/* QDCOUNT == 1 */
	0x00, 0x01,
	/* ANCOUNT == 1 */
	0x00, 0x00,
	/* NSCOUNT == 0 */
	0x00, 0x00,
	/* ARCOUNT == 0 */
	0x00, 0x00,
};

static int generate_dns_header(unsigned short num_records, char *buf)
{
	unsigned short net_num_records = htons(num_records);

	memcpy(buf, DNS_HEADER, ARRAY_LEN(DNS_HEADER));
	/* Overwrite the ANCOUNT with the actual number of answers */
	memcpy(&buf[6], &net_num_records, sizeof(num_records));

	return ARRAY_LEN(DNS_HEADER);
}

const char DNS_QUESTION [] = {
	/* goose */
	0x05, 0x67, 0x6f, 0x6f, 0x73, 0x65,
	/* feathers */
	0x08, 0x66, 0x65, 0x61, 0x74, 0x68, 0x65, 0x72, 0x73,
	/* end label */
	0x00,
	/* SRV type */
	0x00, 0x23,
	/* IN class */
	0x00, 0x01,
};

static int generate_dns_question(char *buf)
{
	memcpy(buf, DNS_QUESTION, ARRAY_LEN(DNS_QUESTION));
	return ARRAY_LEN(DNS_QUESTION);
}

const char SRV_ANSWER [] = {
	/* Domain points to name from question */
	0xc0, 0x0c,
	/* NAPTR type */
	0x00, 0x23,
	/* IN Class */
	0x00, 0x01,
	/* TTL (12345 by default) */
	0x00, 0x00, 0x30, 0x39,
};

static int generate_dns_answer(int ttl, char *buf)
{
	int net_ttl = htonl(ttl);

	memcpy(buf, SRV_ANSWER, ARRAY_LEN(SRV_ANSWER));
	/* Overwrite TTL if one is provided */
	if (ttl) {
		memcpy(&buf[6], &net_ttl, sizeof(int));
	}

	return ARRAY_LEN(SRV_ANSWER);
}

static int write_dns_string(const char *string, char *buf)
{
	uint8_t len = strlen(string);
	buf[0] = len;
	if (len) {
		memcpy(&buf[1], string, len);
	}

	return len + 1;
}

static int write_dns_domain(const char *string, char *buf)
{
	char *copy = ast_strdupa(string);
	char *part;
	char *ptr = buf;

	while ((part = strsep(&copy, "."))) {
		ptr += write_dns_string(part, ptr);
	}
	ptr += write_dns_string("", ptr);

	return ptr - buf;
}

struct srv_record {
	uint16_t priority;
	uint16_t weight;
	uint16_t port;
	const char *host;
	unsigned int ignore_priority;
	unsigned int ignore_weight;
	unsigned int ignore_port;
	unsigned int ignore_host;
};

static int generate_srv_record(struct srv_record *record, char *buf)
{
	uint16_t priority = htons(record->priority);
	uint16_t weight = htons(record->weight);
	uint16_t port = htons(record->port);
	char *ptr = buf;

	if (!record->ignore_priority) {
		memcpy(ptr, &priority, sizeof(priority));
		ptr += sizeof(priority);
	}

	if (!record->ignore_weight) {
		memcpy(ptr, &weight, sizeof(weight));
		ptr += sizeof(weight);
	}

	if (!record->ignore_port) {
		memcpy(ptr, &port, sizeof(port));
		ptr += sizeof(port);
	}

	if (!record->ignore_host) {
		ptr += write_dns_domain(record->host, ptr);
	}

	return ptr - buf;
}

static struct srv_record *test_records;
static int num_test_records;
static char ans_buffer[1024];

static void *srv_thread(void *dns_query)
{
	struct ast_dns_query *query = dns_query;
	int i;
	char *ptr = ans_buffer;

	ptr += generate_dns_header(num_test_records, ptr);
	ptr += generate_dns_question(ptr);

	for (i = 0; i < num_test_records; ++i) {
		unsigned short rdlength;
		unsigned short net_rdlength;

		ptr += generate_dns_answer(0, ptr);
		rdlength = generate_srv_record(&test_records[i], ptr + 2);
		net_rdlength = htons(rdlength);
		memcpy(ptr, &net_rdlength, 2);
		ptr += 2;
		ptr += rdlength;
	}

	ast_dns_resolver_set_result(query, 0, 0, ns_r_noerror, "goose.feathers", ans_buffer, ptr - ans_buffer);

	for (i = 0; i < num_test_records; ++i) {
		char record[128];
		ptr = record;

		ptr += generate_srv_record(&test_records[i], ptr);
		ast_dns_resolver_add_record(query, ns_t_srv, ns_c_in, 12345, record, ptr - record);
	}

	ast_dns_resolver_completed(query);

	ao2_ref(query, -1);
	return NULL;
}

static int srv_resolve(struct ast_dns_query *query)
{
	pthread_t thread;

	return ast_pthread_create_detached(&thread, NULL, srv_thread, ao2_bump(query));
}

static int srv_cancel(struct ast_dns_query *query)
{
	return -1;
}

static struct ast_dns_resolver srv_resolver = {
	.name = "srv_test",
	.priority = 0,
	.resolve = srv_resolve,
	.cancel = srv_cancel,
};

AST_TEST_DEFINE(srv_resolve_single_record)
{
	RAII_VAR(struct ast_dns_result *, result, NULL, ast_dns_result_free);
	const struct ast_dns_record *record;
	struct srv_record records[] = {
		{ 10, 10, 5060, "goose.down" },
	};

	int srv_record_order[] = { 0, };
	enum ast_test_result_state res = AST_TEST_PASS;
	int i;

	switch (cmd) {
	case TEST_INIT:
		info->name = "srv_resolve_single_record";
		info->category = "/main/dns/srv/";
		info->summary = "Test an SRV lookup which returns a single record";
		info->description = "This test defines a single SRV record and performs a\n"
			"resolution of the domain to which they belong. The test ensures that all\n"
			"fields of the SRV record are parsed correctly\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	test_records = records;
	num_test_records = ARRAY_LEN(records);
	memset(ans_buffer, 0, sizeof(ans_buffer));

	ast_dns_resolver_register(&srv_resolver);

	if (ast_dns_resolve("goose.feathers", ns_t_srv, ns_c_in, &result)) {
		ast_test_status_update(test, "DNS resolution failed\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	if (!result) {
		ast_test_status_update(test, "DNS resolution returned no result\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	i = 0;
	for (record = ast_dns_result_get_records(result); record; record = ast_dns_record_get_next(record)) {
		if (ast_dns_srv_get_priority(record) != records[srv_record_order[i]].priority) {
			ast_test_status_update(test, "Unexpected priority in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		if (ast_dns_srv_get_weight(record) != records[srv_record_order[i]].weight) {
			ast_test_status_update(test, "Unexpected weight in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		if (ast_dns_srv_get_port(record) != records[srv_record_order[i]].port) {
			ast_test_status_update(test, "Unexpected port in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		if (strcmp(ast_dns_srv_get_host(record), records[srv_record_order[i]].host)) {
			ast_test_status_update(test, "Unexpected host in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		++i;
	}

	if (i != ARRAY_LEN(records)) {
		ast_test_status_update(test, "Unexpected number of records returned in SRV lookup\n");
		res = AST_TEST_FAIL;
	}

cleanup:

	ast_dns_resolver_unregister(&srv_resolver);

	test_records = NULL;
	num_test_records = 0;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	return res;
}

AST_TEST_DEFINE(srv_resolve_sort_priority)
{
	RAII_VAR(struct ast_dns_result *, result, NULL, ast_dns_result_free);
	const struct ast_dns_record *record;
	struct srv_record records[] = {
		{ 20, 10, 5060, "tacos" },
		{ 10, 10, 5060, "goose.down" },
	};

	int srv_record_order[] = { 1, 0};
	enum ast_test_result_state res = AST_TEST_PASS;
	int i;

	switch (cmd) {
	case TEST_INIT:
		info->name = "srv_resolve_sort_priority";
		info->category = "/main/dns/srv/";
		info->summary = "Test an SRV lookup which returns two records with differing priorities";
		info->description = "This test defines two SRV records with differing priorities and\n"
			"performs a resolution of the domain to which they belong. The test ensures that\n"
			"the two records are sorted according to priority and that all fields of the SRV\n"
			"records are parsed correctly\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	test_records = records;
	num_test_records = ARRAY_LEN(records);
	memset(ans_buffer, 0, sizeof(ans_buffer));

	ast_dns_resolver_register(&srv_resolver);

	if (ast_dns_resolve("goose.feathers", ns_t_srv, ns_c_in, &result)) {
		ast_test_status_update(test, "DNS resolution failed\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	if (!result) {
		ast_test_status_update(test, "DNS resolution returned no result\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	i = 0;
	for (record = ast_dns_result_get_records(result); record; record = ast_dns_record_get_next(record)) {
		if (ast_dns_srv_get_priority(record) != records[srv_record_order[i]].priority) {
			ast_test_status_update(test, "Unexpected priority in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		if (ast_dns_srv_get_weight(record) != records[srv_record_order[i]].weight) {
			ast_test_status_update(test, "Unexpected weight in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		if (ast_dns_srv_get_port(record) != records[srv_record_order[i]].port) {
			ast_test_status_update(test, "Unexpected port in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		if (strcmp(ast_dns_srv_get_host(record), records[srv_record_order[i]].host)) {
			ast_test_status_update(test, "Unexpected host in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		++i;
	}

	if (i != ARRAY_LEN(records)) {
		ast_test_status_update(test, "Unexpected number of records returned in SRV lookup\n");
		res = AST_TEST_FAIL;
	}

cleanup:

	ast_dns_resolver_unregister(&srv_resolver);

	test_records = NULL;
	num_test_records = 0;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	return res;
}

AST_TEST_DEFINE(srv_resolve_same_priority_zero_weight)
{
	RAII_VAR(struct ast_dns_result *, result, NULL, ast_dns_result_free);
	const struct ast_dns_record *record;
	struct srv_record records[] = {
		{ 10, 0, 5060, "tacos" },
		{ 10, 10, 5060, "goose.down" },
	};

	int srv_record_order[] = { 1, 0};
	enum ast_test_result_state res = AST_TEST_PASS;
	int i;

	switch (cmd) {
	case TEST_INIT:
		info->name = "srv_resolve_same_priority_zero_weight";
		info->category = "/main/dns/srv/";
		info->summary = "Test an SRV lookup which returns two records with same priority but different weights";
		info->description = "This test defines two SRV records with same priority but different weights and\n"
			"performs a resolution of the domain to which they belong. The test ensures that\n"
			"the record with zero weight comes last and that all fields of the SRV\n"
			"records are parsed correctly\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	test_records = records;
	num_test_records = ARRAY_LEN(records);
	memset(ans_buffer, 0, sizeof(ans_buffer));

	ast_dns_resolver_register(&srv_resolver);

	if (ast_dns_resolve("goose.feathers", ns_t_srv, ns_c_in, &result)) {
		ast_test_status_update(test, "DNS resolution failed\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	if (!result) {
		ast_test_status_update(test, "DNS resolution returned no result\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	i = 0;
	for (record = ast_dns_result_get_records(result); record; record = ast_dns_record_get_next(record)) {
		if (ast_dns_srv_get_priority(record) != records[srv_record_order[i]].priority) {
			ast_test_status_update(test, "Unexpected priority in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		if (ast_dns_srv_get_weight(record) != records[srv_record_order[i]].weight) {
			ast_test_status_update(test, "Unexpected weight in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		if (ast_dns_srv_get_port(record) != records[srv_record_order[i]].port) {
			ast_test_status_update(test, "Unexpected port in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		if (strcmp(ast_dns_srv_get_host(record), records[srv_record_order[i]].host)) {
			ast_test_status_update(test, "Unexpected host in returned SRV record\n");
			res = AST_TEST_FAIL;
		}
		++i;
	}

	if (i != ARRAY_LEN(records)) {
		ast_test_status_update(test, "Unexpected number of records returned in SRV lookup\n");
		res = AST_TEST_FAIL;
	}

cleanup:

	ast_dns_resolver_unregister(&srv_resolver);

	test_records = NULL;
	num_test_records = 0;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	return res;
}

AST_TEST_DEFINE(srv_resolve_same_priority_different_weights)
{
	struct srv_record records[] = {
		{ 10, 10, 5060, "tacos" },
		{ 10, 20, 5060, "goose.down" },
	};

	int srv_record_occurence[2] = { 0, };
	enum ast_test_result_state res = AST_TEST_PASS;
	int count = 0;

	switch (cmd) {
	case TEST_INIT:
		info->name = "srv_resolve_same_priority_different_weights";
		info->category = "/main/dns/srv/";
		info->summary = "Test an SRV lookup which returns two records with same priority but different weights";
		info->description = "This test defines two SRV records with same priority but different weights and\n"
			"performs a resolution of the domain to which they belong. The test ensures that\n"
			"the record with higher weight occurs more often than the one of lesser weight\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	test_records = records;
	num_test_records = ARRAY_LEN(records);

	ast_dns_resolver_register(&srv_resolver);

	for (count = 0; count < 100; count++) {
		struct ast_dns_result *result;
		const struct ast_dns_record *record;
		int i;

		memset(ans_buffer, 0, sizeof(ans_buffer));

		if (ast_dns_resolve("goose.feathers", ns_t_srv, ns_c_in, &result)) {
			ast_test_status_update(test, "DNS resolution failed\n");
			res = AST_TEST_FAIL;
			goto cleanup;
		}

		if (!result) {
			ast_test_status_update(test, "DNS resolution returned no result\n");
			res = AST_TEST_FAIL;
			goto cleanup;
		}

		record = ast_dns_result_get_records(result);
		for (i = 0; i < ARRAY_LEN(records); i++) {
			if (ast_dns_srv_get_priority(record) != records[i].priority) {
				continue;
			}
			if (ast_dns_srv_get_weight(record) != records[i].weight) {
				continue;
			}
			if (ast_dns_srv_get_port(record) != records[i].port) {
				continue;
			}
			if (strcmp(ast_dns_srv_get_host(record), records[i].host)) {
				continue;
			}

			srv_record_occurence[i]++;
			break;
		}

		ast_dns_result_free(result);
	}

	if (srv_record_occurence[0] > srv_record_occurence[1]) {
		ast_test_status_update(test, "SRV sorting resulted in lesser weight being returned more often\n");
		res = AST_TEST_FAIL;
	}

cleanup:

	ast_dns_resolver_unregister(&srv_resolver);

	test_records = NULL;
	num_test_records = 0;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	return res;
}

AST_TEST_DEFINE(srv_resolve_different_priorities_different_weights)
{
	struct srv_record records[] = {
		{ 10, 10, 5060, "tacos" },
		{ 10, 20, 5060, "goose.down" },
		{ 5, 80, 5060, "moo" },
		{ 5, 10, 5060, "Canada" },
	};

	int srv_record_priority[4] = { 5, 5, 10, 10 };
	int srv_record_occurence[4] = { 0, };
	enum ast_test_result_state res = AST_TEST_PASS;
	int count = 0;

	switch (cmd) {
	case TEST_INIT:
		info->name = "srv_resolve_different_priorities_different_weights";
		info->category = "/main/dns/srv/";
		info->summary = "Test an SRV lookup which returns four records with different priority and different weights";
		info->description = "This test defines four SRV records, two with one priority and two with another priority,\n"
			"and different weights and performs a resolution of the domain to which they belong.\n"
			"The test ensures that the priorities are sorted properly and that the records with higher weight\n"
			"occur more often than the ones of less weight.\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	test_records = records;
	num_test_records = ARRAY_LEN(records);

	ast_dns_resolver_register(&srv_resolver);

	for (count = 0; count < 100; count++) {
		struct ast_dns_result *result;
		const struct ast_dns_record *record;
		int i;

		memset(ans_buffer, 0, sizeof(ans_buffer));

		if (ast_dns_resolve("goose.feathers", ns_t_srv, ns_c_in, &result)) {
			ast_test_status_update(test, "DNS resolution failed\n");
			res = AST_TEST_FAIL;
			goto cleanup;
		}

		if (!result) {
			ast_test_status_update(test, "DNS resolution returned no result\n");
			res = AST_TEST_FAIL;
			goto cleanup;
		}

		i = 0;
		for (record = ast_dns_result_get_records(result); record; record = ast_dns_record_get_next(record)) {
			if (ast_dns_srv_get_priority(record) != srv_record_priority[i]) {
				ast_test_status_update(test, "Unexpected priority in returned SRV record\n");
				res = AST_TEST_FAIL;
			}
			i++;
		}

		record = ast_dns_result_get_records(result);
		for (i = 0; i < ARRAY_LEN(records); i++) {
			if (ast_dns_srv_get_priority(record) != records[i].priority) {
				continue;
			}
			if (ast_dns_srv_get_weight(record) != records[i].weight) {
				continue;
			}
			if (ast_dns_srv_get_port(record) != records[i].port) {
				continue;
			}
			if (strcmp(ast_dns_srv_get_host(record), records[i].host)) {
				continue;
			}

			srv_record_occurence[i]++;
			break;
		}

		ast_dns_result_free(result);
	}

	if (srv_record_occurence[0] > srv_record_occurence[1]) {
		ast_test_status_update(test, "SRV sorting resulted in lesser weight being returned more often for priority 10\n");
		res = AST_TEST_FAIL;
	}

	if (srv_record_occurence[3] > srv_record_occurence[2]) {
		ast_test_status_update(test, "SRV sorting resulted in lesser weight being returned more often for priority 5\n");
		res = AST_TEST_FAIL;
	}

cleanup:

	ast_dns_resolver_unregister(&srv_resolver);

	test_records = NULL;
	num_test_records = 0;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	return res;
}

AST_TEST_DEFINE(srv_resolve_record_missing_weight_port_host)
{
	RAII_VAR(struct ast_dns_result *, result, NULL, ast_dns_result_free);
	const struct ast_dns_record *record;
	struct srv_record records[] = {
		{ 10, 10, 5060, "tacos.com", 0, 1, 1, 1 },
	};

	enum ast_test_result_state res = AST_TEST_PASS;

	switch (cmd) {
	case TEST_INIT:
		info->name = "srv_resolve_record_missing_weight_port_host";
		info->category = "/main/dns/srv/";
		info->summary = "Test an SRV lookup which returns a single invalid record";
		info->description = "This test defines a single SRV record and performs a\n"
			"resolution of the domain to which they belong. The test ensures that the\n"
			"record is determined to be corrupt as it contains only a priority\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	test_records = records;
	num_test_records = ARRAY_LEN(records);
	memset(ans_buffer, 0, sizeof(ans_buffer));

	ast_dns_resolver_register(&srv_resolver);

	if (ast_dns_resolve("goose.feathers", ns_t_srv, ns_c_in, &result)) {
		ast_test_status_update(test, "DNS resolution failed\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	if (!result) {
		ast_test_status_update(test, "DNS resolution returned no result\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	record = ast_dns_result_get_records(result);
	if (record) {
		ast_test_status_update(test, "Unexpected record returned from SRV query\n");
		res = AST_TEST_FAIL;
	}

cleanup:

	ast_dns_resolver_unregister(&srv_resolver);

	test_records = NULL;
	num_test_records = 0;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	return res;
}

AST_TEST_DEFINE(srv_resolve_record_missing_port_host)
{
	RAII_VAR(struct ast_dns_result *, result, NULL, ast_dns_result_free);
	const struct ast_dns_record *record;
	struct srv_record records[] = {
		{ 10, 10, 5060, "tacos.com", 0, 0, 1, 1 },
	};

	enum ast_test_result_state res = AST_TEST_PASS;

	switch (cmd) {
	case TEST_INIT:
		info->name = "srv_resolve_record_missing_port_host";
		info->category = "/main/dns/srv/";
		info->summary = "Test an SRV lookup which returns a single invalid record";
		info->description = "This test defines a single SRV record and performs a\n"
			"resolution of the domain to which they belong. The test ensures that the\n"
			"record is determined to be corrupt as it contains only a priority and weight\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	test_records = records;
	num_test_records = ARRAY_LEN(records);
	memset(ans_buffer, 0, sizeof(ans_buffer));

	ast_dns_resolver_register(&srv_resolver);

	if (ast_dns_resolve("goose.feathers", ns_t_srv, ns_c_in, &result)) {
		ast_test_status_update(test, "DNS resolution failed\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	if (!result) {
		ast_test_status_update(test, "DNS resolution returned no result\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	record = ast_dns_result_get_records(result);
	if (record) {
		ast_test_status_update(test, "Unexpected record returned from SRV query\n");
		res = AST_TEST_FAIL;
	}

cleanup:

	ast_dns_resolver_unregister(&srv_resolver);

	test_records = NULL;
	num_test_records = 0;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	return res;
}

AST_TEST_DEFINE(srv_resolve_record_missing_host)
{
	RAII_VAR(struct ast_dns_result *, result, NULL, ast_dns_result_free);
	const struct ast_dns_record *record;
	struct srv_record records[] = {
		{ 10, 10, 5060, "tacos.com", 0, 0, 0, 1 },
	};

	enum ast_test_result_state res = AST_TEST_PASS;

	switch (cmd) {
	case TEST_INIT:
		info->name = "srv_resolve_record_missing_host";
		info->category = "/main/dns/srv/";
		info->summary = "Test an SRV lookup which returns a single invalid record";
		info->description = "This test defines a single SRV record and performs a\n"
			"resolution of the domain to which they belong. The test ensures that the\n"
			"record is determined to be corrupt as it contains only a priority, weight,\n"
			"and port\n";
		return AST_TEST_NOT_RUN;
	case TEST_EXECUTE:
		break;
	}

	test_records = records;
	num_test_records = ARRAY_LEN(records);
	memset(ans_buffer, 0, sizeof(ans_buffer));

	ast_dns_resolver_register(&srv_resolver);

	if (ast_dns_resolve("goose.feathers", ns_t_srv, ns_c_in, &result)) {
		ast_test_status_update(test, "DNS resolution failed\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	if (!result) {
		ast_test_status_update(test, "DNS resolution returned no result\n");
		res = AST_TEST_FAIL;
		goto cleanup;
	}

	record = ast_dns_result_get_records(result);
	if (record) {
		ast_test_status_update(test, "Unexpected record returned from SRV query\n");
		res = AST_TEST_FAIL;
	}

cleanup:

	ast_dns_resolver_unregister(&srv_resolver);

	test_records = NULL;
	num_test_records = 0;
	memset(ans_buffer, 0, sizeof(ans_buffer));

	return res;
}

static int unload_module(void)
{
	AST_TEST_UNREGISTER(srv_resolve_single_record);
	AST_TEST_UNREGISTER(srv_resolve_sort_priority);
	AST_TEST_UNREGISTER(srv_resolve_same_priority_zero_weight);
	AST_TEST_UNREGISTER(srv_resolve_same_priority_different_weights);
	AST_TEST_UNREGISTER(srv_resolve_different_priorities_different_weights);
	AST_TEST_UNREGISTER(srv_resolve_record_missing_weight_port_host);
	AST_TEST_UNREGISTER(srv_resolve_record_missing_port_host);
	AST_TEST_UNREGISTER(srv_resolve_record_missing_host);

	return 0;
}

static int load_module(void)
{
	AST_TEST_REGISTER(srv_resolve_single_record);
	AST_TEST_REGISTER(srv_resolve_sort_priority);
	AST_TEST_REGISTER(srv_resolve_same_priority_zero_weight);
	AST_TEST_REGISTER(srv_resolve_same_priority_different_weights);
	AST_TEST_REGISTER(srv_resolve_different_priorities_different_weights);
	AST_TEST_REGISTER(srv_resolve_record_missing_weight_port_host);
	AST_TEST_REGISTER(srv_resolve_record_missing_port_host);
	AST_TEST_REGISTER(srv_resolve_record_missing_host);

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "DNS SRV Tests");