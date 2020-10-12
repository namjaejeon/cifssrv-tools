// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2020 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@lists.sourceforge.net
 */

#include <memory.h>
#include <endian.h>
#include <glib.h>
#include <pwd.h>
#include <errno.h>
#include <linux/ksmbd_server.h>

#include <management/user.h>

#include <rpc.h>
#include <rpc_samr.h>
#include <smbacl.h>
#include <ksmbdtools.h>

#define SAMR_OPNUM_CONNECT5		64
#define SAMR_OPNUM_ENUM_DOMAIN		6
#define SAMR_OPNUM_LOOKUP_DOMAIN	5
#define SAMR_OPNUM_OPEN_DOMAIN		7
#define SAMR_OPNUM_LOOKUP_NAMES		17
#define SAMR_OPNUM_OPEN_USER		34
#define SAMR_OPNUM_QUERY_USER_INFO	36
#define SAMR_OPNUM_QUERY_SECURITY	3
#define SAMR_OPNUM_CLOSE		1

static GHashTable	*ch_table;
static GRWLock		ch_table_lock;

static void samr_ch_free(struct connect_handle *ch)
{
	if (ch->handle != (unsigned int)-1) {
		g_rw_lock_writer_lock(&ch_table_lock);
		g_hash_table_remove(ch_table, &(ch->handle));
		g_rw_lock_writer_unlock(&ch_table_lock);
	}

	free(ch);
}

static struct connect_handle *samr_ch_alloc(void)
{
	struct connect_handle *ch;
	int ret;

	ch = calloc(1, sizeof(struct connect_handle));
	if (!ch)
		return NULL;

	ch->handle = 1;
	g_rw_lock_writer_lock(&ch_table_lock);
	ret = g_hash_table_insert(ch_table, &(ch->handle), ch);
	g_rw_lock_writer_unlock(&ch_table_lock);

	if (!ret) {
		ch->handle = (unsigned int)-1;
		samr_ch_free(ch);
		ch = NULL;
	}

	return ch;
}

static struct connect_handle *samr_ch_lookup(unsigned int handle)
{
	struct connect_handle *ch;

	g_rw_lock_reader_lock(&ch_table_lock);
	ch = g_hash_table_lookup(ch_table, &handle);
	g_rw_lock_reader_unlock(&ch_table_lock);

	return ch;
}

static int __domain_entry_processed(struct ksmbd_rpc_pipe *pipe, int i)
{
	char *name;

//	name = g_array_index(pipe->entries, gpointer, i);
//	pipe->entries = g_array_remove_index(pipe->entries, i);
//	pipe->num_entries--;
	pipe->num_processed++;
//	kfree(name);

	return 0;
}

static int samr_connect5_invoke(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct ndr_uniq_char_ptr server_name;

	ndr_read_uniq_vsting_ptr(dce, &server_name);
	ndr_read_int32(dce); // Read Access mask
	dce->sm_req.level = ndr_read_int32(dce); // Read level in
	ndr_read_int32(dce); // Read Info in
	dce->sm_req.client_version = ndr_read_int32(dce);
	return 0;
}

static int samr_connect5_return(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch;

	ndr_write_union_int32(dce, dce->sm_req.level); //level out
	ndr_write_int32(dce, dce->sm_req.client_version); //client version
	ndr_write_int32(dce, 0); //reserved

	ch = samr_ch_alloc();
	if (!ch)
		return KSMBD_RPC_ENOMEM;

	/* write connect handle */
	ndr_write_int64(dce, (__u64)ch->handle);
	ndr_write_int64(dce, (__u64)ch->handle);

	return KSMBD_RPC_OK;
}

static int samr_enum_domain_invoke(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
//	char *hostname, *builtin;
	char *hostname;
	struct connect_handle *ch;
	unsigned long long id;

	id = ndr_read_int64(dce);
	ch = samr_ch_lookup(id);
	if (!ch)
		return KSMBD_RPC_EBAD_FID;

	/*
	 * ksmbd supports the standalone server and
	 * uses the hostname as the domain name.
	 */

	hostname = malloc(256);
	if (!hostname)
		return KSMBD_RPC_ENOMEM; 
//	builtin = kmalloc(8, GFP_KERNEL);
//	if (!builtin)
//		return KSMBD_RPC_ENOMEM; 

	gethostname(hostname, 256);
//	strcpy(builtin, "Builtin");

	pipe->entries = g_array_append_val(pipe->entries, hostname);
//	pipe->entries = g_array_append_val(pipe->entries, builtin);
//	pipe->num_entries = 2;
	pipe->num_entries = 1;
	pipe->entry_processed = __domain_entry_processed;
}

int samr_ndr_write_domain_array(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	int i, ret = 0;

	for (i = 0; i < pipe->num_entries; i++) {
		gpointer entry;
		int name_len;

		ret = ndr_write_int32(dce, i);
		entry = g_array_index(pipe->entries, gpointer, i);
		name_len = strlen((char *)entry);
		ret = ndr_write_int32(dce, name_len);
		ret = ndr_write_int32(dce, name_len);

		dce->num_pointers++;
		ret = ndr_write_int32(dce, dce->num_pointers); /* ref pointer for name entry*/
	}

	for (i = 0; i < pipe->num_entries; i++) {
		gpointer entry;

		entry = g_array_index(pipe->entries,  gpointer, i);
		ret = ndr_write_vstring(dce, (char *)entry);
	}

	if (pipe->entry_processed) {
		for (i = 0; i < pipe->num_entries; i++)
			pipe->entry_processed(pipe, 0);
	}

	return ret;
}

static int samr_enum_domain_return(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	int status = KSMBD_RPC_OK;

	/* Resume Handle */
	ndr_write_int32(dce, 0);

	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers); /* ref pointer */
	ndr_write_int32(dce, pipe->num_entries); /* Sam entry count */
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers); /* ref pointer */
	ndr_write_int32(dce, pipe->num_entries); /* Sam max entry count */

	status = samr_ndr_write_domain_array(pipe);

	/* [out] DWORD* Num Entries */
	ndr_write_int32(dce, pipe->num_processed);

	return status;
}

static int samr_lookup_domain_invoke(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch;
	unsigned long long id;

	id = ndr_read_int64(dce);
	ndr_read_int64(dce);

	ch = samr_ch_lookup(id);
	if (!ch)
		return KSMBD_RPC_EBAD_FID;

	// name len
	ndr_read_int16(dce);
	// name size
	ndr_read_int16(dce);
	// read domain name
	ndr_read_uniq_vsting_ptr(dce, &dce->sm_req.lookup_name);

	return KSMBD_RPC_OK;
}

static int samr_lookup_domain_return(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch = dce->sm_req.ch;
	struct smb_sid sid = {0};
	int i, j;

	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);
	ndr_write_int32(dce, 4);

	for (i = 0; i < pipe->num_entries; i++) {
		gpointer entry;

		entry = g_array_index(pipe->entries, gpointer, i);
		if (!strcmp(STR_VAL(dce->sm_req.lookup_name), (char *)entry)) {
			smb_init_sid(dce, &sid);
			smb_write_sid(dce, &sid);
			smb_copy_sid(&ch->sid, &sid);
			ch->domain_name =
				malloc(strlen(STR_VAL(dce->sm_req.lookup_name)));
			if (!ch->domain_name)
				return KSMBD_RPC_ENOMEM;

			strcpy(ch->domain_name,
				STR_VAL(dce->sm_req.lookup_name));
		}
	}

	return KSMBD_RPC_OK;
}

static int samr_open_domain_invoke(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch;
	int i, j;
	unsigned long long id;
	struct smb_sid sid;

	id = ndr_read_int64(dce);
	ch = samr_ch_lookup(id);
	if (!ch)
		return KSMBD_RPC_EBAD_FID;

	dce->sm_req.ch = ch;

	/* Acccess Mask */
	ndr_read_int32(dce);
	/* Count */
	ndr_read_int32(dce);

	smb_read_sid(dce, &sid);

	if (smb_compare_sids(&sid, &ch->sid))
		return KSMBD_RPC_EBAD_FID;

	return KSMBD_RPC_OK;
}

static int samr_open_domain_return(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch = dce->sm_req.ch;

	ndr_write_int64(dce, (__u64)ch->handle);
	ndr_write_int64(dce, (__u64)ch->handle);

	return KSMBD_RPC_OK;
}

static int samr_lookup_names_invoke(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch;
	struct ndr_uniq_char_ptr username;
	struct ksmbd_user *user;
	struct passwd *passwd;
	unsigned long long id;
	int user_num;

	id = ndr_read_int64(dce);
	ch = samr_ch_lookup(id);
	if (!ch)
		return KSMBD_RPC_EBAD_FID;

	dce->sm_req.ch = ch;

	user_num = ndr_read_int32(dce);
	/* Max Count */
	ndr_read_int32(dce);
	/* Offset */
	ndr_read_int32(dce);
	/* Actual Count */
	ndr_read_int32(dce);

	/* Name Len */
	ndr_read_int16(dce);
	/* Name Size */
	ndr_read_int16(dce);

	/* Names */
	ndr_read_uniq_vsting_ptr(dce, &username);

	passwd = getpwnam(STR_VAL(username));
	if (!passwd)
		return KSMBD_RPC_EACCESS_DENIED;
	ch->rid = passwd->pw_uid;

	user = usm_lookup_user(STR_VAL(username));
	if (!user)
		return KSMBD_RPC_EACCESS_DENIED;

	ch->user = user;

	return KSMBD_RPC_OK;
}

static int samr_lookup_names_return(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch = dce->sm_req.ch;
	struct passwd *passwd;

	//samr IDs
	ndr_write_int32(dce, 1);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);
	ndr_write_int32(dce, 1);
	ndr_write_int32(dce, ch->rid);

	// Need to check if it is needed
	ndr_write_int32(dce, 1);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);
	ndr_write_int32(dce, 1);
	ndr_write_int32(dce, 1);

	return KSMBD_RPC_OK;
}

static int samr_open_user_invoke(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch;
	unsigned long long id;
	unsigned int req_rid;

	id = ndr_read_int64(dce);
	ch = samr_ch_lookup(id);
	if (!ch)
		return KSMBD_RPC_EBAD_FID;
	dce->sm_req.ch = ch;

	ndr_read_int32(dce);
	req_rid = ndr_read_int32(dce);

	if (req_rid != ch->rid)
		return KSMBD_RPC_EBAD_FID;

	return KSMBD_RPC_OK;
}

static int samr_open_user_return(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch = dce->sm_req.ch;

	ndr_write_int64(dce, (__u64)ch->handle);
	ndr_write_int64(dce, (__u64)ch->handle);
	return KSMBD_RPC_OK;
}

static int samr_query_user_info_invoke(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch;
	unsigned long long id;

	id = ndr_read_int64(dce);
	ch = samr_ch_lookup(id);
	if (!ch)
		return KSMBD_RPC_EBAD_FID;
	dce->sm_req.ch = ch;

	return KSMBD_RPC_OK;
}

static int samr_query_user_info_return(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch = dce->sm_req.ch;
	char *home_dir, *profile_path;
	int home_dir_len = 2 + strlen(ch->domain_name) + 1 + strlen(ch->user->name);
	int profile_path_len = home_dir_len + strlen("profile");
	int i;

	home_dir = calloc(1, home_dir_len);
	if (!home_dir)
		return KSMBD_RPC_EBAD_FID;

	strcpy(home_dir, "\\\\");
	strcat(home_dir, ch->domain_name);
	strcat(home_dir, "\\");
	strcat(home_dir, ch->user->name);

	profile_path = calloc(1, profile_path_len);
	if (!profile_path)
		return KSMBD_RPC_EBAD_FID;

	strcat(profile_path, "\\");
	strcat(profile_path, "profile");

	/* Ref ID */
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Info */
	ndr_write_int16(dce, 15);

	/* Last Logon time */
	ndr_write_int64(dce, 0);
	/* Last Logoff time */
	ndr_write_int64(dce, 0);
	/* Last Password Change */
	ndr_write_int64(dce, 0);
	/* Acct Expiry */
	ndr_write_int64(dce, 0);
	/* Allow Password Change */
	ndr_write_int64(dce, 0);
	/* Force Password Change */
	ndr_write_int64(dce, 0);

	/* Account Name */
	ndr_write_int16(dce, strlen(ch->user->name)*2);
	ndr_write_int16(dce, strlen(ch->user->name)*2);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Full Name */
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Home Directory */
	ndr_write_int16(dce, strlen(home_dir)*2);
	ndr_write_int16(dce, strlen(home_dir)*2);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Home Drive */
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Logon Script */
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* profile path */
	ndr_write_int16(dce, strlen(profile_path)*2);
	ndr_write_int16(dce, strlen(profile_path)*2);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Description */
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Workstations */
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Comment */
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Parameters */
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Lm, Nt, Password and Private*/
	for (i = 0; i < 3; i++) {
		ndr_write_int16(dce, 0);
		ndr_write_int16(dce, 0);
		ndr_write_int32(dce, 0);
	}

	/* Buf Count */
	ndr_write_int32(dce, 0);
	/* Pointer to Buffer */
	ndr_write_int32(dce, 0);
	/* RID */
	ndr_write_int32(dce, ch->rid);
	/* Primary Gid */
	ndr_write_int32(dce, 513);

	/* Acct Flags : Acb Normal */
	ndr_write_int32(dce, 0x00000010);

	/* Fields Present */
	ndr_write_int32(dce, 0x00FFFFFF);

	/* Logon Hours */
	ndr_write_int16(dce, 168);
	ndr_write_int16(dce, 0);

	/* Pointers to Bits */
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);

	/* Bad Password/Logon Count/Country Code/Code Page */
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);
	ndr_write_int16(dce, 0);

	/* Lm/Nt Password Set, Password Expired/etc */
	ndr_write_int8(dce, 0);
	ndr_write_int8(dce, 0);
	ndr_write_int8(dce, 0);
	ndr_write_int8(dce, 0);

	return KSMBD_RPC_OK;
}

static int samr_query_security_invoke(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch;
	unsigned long long id;

	id = ndr_read_int64(dce);
	ch = samr_ch_lookup(id);
	if (!ch)
		return KSMBD_RPC_EBAD_FID;
	dce->sm_req.ch = ch;

	return KSMBD_RPC_OK;
}

static int samr_query_security_return(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch = dce->sm_req.ch;
	int sec_desc_len, offset;

	build_sec_desc(dce, &sec_desc_len, ch->rid);
	offset = dce->offset;

	dce->offset = 0;
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);
	ndr_write_int32(dce, sec_desc_len);
	
	dce->num_pointers++;
	ndr_write_int32(dce, dce->num_pointers);
	ndr_write_int32(dce, sec_desc_len);

	dce->offset = offset;

	return KSMBD_RPC_OK;
}

static int samr_close_invoke(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch;
	unsigned long long id;

	id = ndr_read_int64(dce);
	ch = samr_ch_lookup(id);
	if (!ch)
		return KSMBD_RPC_EBAD_FID;
	dce->sm_req.ch = ch;

	return KSMBD_RPC_OK;
}

static int samr_close_return(struct ksmbd_rpc_pipe *pipe)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	struct connect_handle *ch = dce->sm_req.ch;
	int i;

	samr_ch_free(ch);

	for (i = 0; i < pipe->num_entries; i++) {
		gpointer entry;

		entry = g_array_index(pipe->entries, gpointer, i);
		pipe->entries = g_array_remove_index(pipe->entries, i);
		free(entry);
	}

	return KSMBD_RPC_OK;
}

static int samr_invoke(struct ksmbd_rpc_pipe *pipe)
{
	int ret = KSMBD_RPC_ENOTIMPLEMENTED;

	switch (pipe->dce->req_hdr.opnum) {
	case SAMR_OPNUM_CONNECT5:
		ret = samr_connect5_invoke(pipe);
		break;
	case SAMR_OPNUM_ENUM_DOMAIN:
		ret = samr_enum_domain_invoke(pipe);
		break;
	case SAMR_OPNUM_LOOKUP_DOMAIN:
		ret = samr_lookup_domain_invoke(pipe);
		break;
	case SAMR_OPNUM_OPEN_DOMAIN:
		ret = samr_open_domain_invoke(pipe);
		break;
	case SAMR_OPNUM_LOOKUP_NAMES:
		ret = samr_lookup_names_invoke(pipe);
		break;
	case SAMR_OPNUM_OPEN_USER:
		ret = samr_open_user_invoke(pipe);
		break;
	case SAMR_OPNUM_QUERY_USER_INFO:
		ret = samr_query_user_info_invoke(pipe);
		break;
	case SAMR_OPNUM_CLOSE:
		ret = samr_close_invoke(pipe);
		break;
	default:
		pr_err("SAMR: unsupported INVOKE method %d\n",
		       pipe->dce->req_hdr.opnum);
		break;
	}

	return ret;
}

static int samr_return(struct ksmbd_rpc_pipe *pipe,
			 struct ksmbd_rpc_command *resp,
			 int max_resp_sz)
{
	struct ksmbd_dcerpc *dce = pipe->dce;
	int ret;
	int status = KSMBD_RPC_ENOTIMPLEMENTED;

	/*
	 * Reserve space for response NDR header. We don't know yet if
	 * the payload buffer is big enough. This will determine if we
	 * can set DCERPC_PFC_FIRST_FRAG|DCERPC_PFC_LAST_FRAG or if we
	 * will have a multi-part response.
	 */
	dce->offset = sizeof(struct dcerpc_header);
	dce->offset += sizeof(struct dcerpc_response_header);
	pipe->num_processed = 0;

	switch (dce->req_hdr.opnum) {
	case SAMR_OPNUM_CONNECT5:
		status = samr_connect5_return(pipe);
		break;
	case SAMR_OPNUM_ENUM_DOMAIN:
		status = samr_enum_domain_return(pipe);
		break;
	case SAMR_OPNUM_LOOKUP_DOMAIN:
		status = samr_lookup_domain_return(pipe);
		break;
	case SAMR_OPNUM_OPEN_DOMAIN:
		status = samr_open_domain_return(pipe);
		break;
	case SAMR_OPNUM_LOOKUP_NAMES:
		status = samr_lookup_names_return(pipe);
		break;
	case SAMR_OPNUM_OPEN_USER:
		status = samr_open_user_return(pipe);
		break;
	case SAMR_OPNUM_QUERY_USER_INFO:
		status = samr_query_user_info_return(pipe);
		break;
	case SAMR_OPNUM_QUERY_SECURITY:
		status = samr_query_user_info_return(pipe);
		break;
	case SAMR_OPNUM_CLOSE:
		status = samr_close_return(pipe);
		break;
	default:
		pr_err("SAMR: unsupported RETURN method %d\n",
			dce->req_hdr.opnum);
		ret = KSMBD_RPC_EBAD_FUNC;
		break;
	}

	if (rpc_restricted_context(dce->rpc_req))
		status = KSMBD_RPC_EACCESS_DENIED;

	/*
	 * [out] DWORD Return value/code
	 */
	ndr_write_int32(dce, status);
	dcerpc_write_headers(dce, status);

	dce->rpc_resp->payload_sz = dce->offset;
	return ret;
}

int rpc_samr_read_request(struct ksmbd_rpc_pipe *pipe,
			    struct ksmbd_rpc_command *resp,
			    int max_resp_sz)
{
	return samr_return(pipe, resp, max_resp_sz);
}

int rpc_samr_write_request(struct ksmbd_rpc_pipe *pipe)
{
	return samr_invoke(pipe);
}