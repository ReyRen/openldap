/* search.c - ldap backend search function */
/* $OpenLDAP$ */
/*
 * Copyright 1998-1999 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/* This is an altered version */
/*
 * Copyright 1999, Howard Chu, All rights reserved. <hyc@highlandsun.com>
 * 
 * Permission is granted to anyone to use this software for any purpose
 * on any computer system, and to alter it and redistribute it, subject
 * to the following restrictions:
 * 
 * 1. The author is not responsible for the consequences of use of this
 *    software, no matter how awful, even if they arise from flaws in it.
 * 
 * 2. The origin of this software must not be misrepresented, either by
 *    explicit claim or by omission.  Since few users ever read sources,
 *    credits should appear in the documentation.
 * 
 * 3. Altered versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.  Since few users
 *    ever read sources, credits should appear in the documentation.
 * 
 * 4. This notice may not be removed or altered.
 *
 *
 *
 * Copyright 2000, Pierangelo Masarati, All rights reserved. <ando@sys-net.it>
 * 
 * This software is being modified by Pierangelo Masarati.
 * The previously reported conditions apply to the modified code as well.
 * Changes in the original code are highlighted where required.
 * Credits for the original code go to the author, Howard Chu.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>

#include "slap.h"
#include "back-ldap.h"

static void ldap_send_entry( Backend *be, Operation *op, struct ldapconn *lc,
                             LDAPMessage *e, struct berval **attrs, int attrsonly );

int
ldap_back_search(
    Backend	*be,
    Connection	*conn,
    Operation	*op,
    const char	*base,
    const char	*nbase,
    int		scope,
    int		deref,
    int		slimit,
    int		tlimit,
    Filter	*filter,
    const char	*filterstr,
    struct berval	**attrs,
    int		attrsonly
)
{
	struct ldapinfo	*li = (struct ldapinfo *) be->be_private;
	struct ldapconn *lc;
	struct timeval	tv;
	LDAPMessage		*res, *e;
	int	count, rc = 0, msgid, sres = LDAP_SUCCESS; 
	char *match = NULL, *err = NULL;
	char *mbase = NULL, *mapped_filter = NULL, **mapped_attrs = NULL;
#ifdef ENABLE_REWRITE
	char *mfilter = NULL, *mmatch = NULL;
#endif /* ENABLE_REWRITE */
	struct slap_limits_set *limit = NULL;
	int isroot = 0;

	lc = ldap_back_getconn(li, conn, op);
	if ( !lc ) {
		return( -1 );
	}

	/* if not root, get appropriate limits */
	if ( be_isroot( be, &op->o_ndn ) ) {
		isroot = 1;
	} else {
		( void ) get_limits( be, op->o_ndn.bv_val, &limit );
	}
	
	/* if no time limit requested, rely on remote server limits */
	/* if requested limit higher than hard limit, abort */
	if ( !isroot && tlimit > limit->lms_t_hard ) {
		/* no hard limit means use soft instead */
		if ( limit->lms_t_hard == 0 ) {
			tlimit = limit->lms_t_soft;
			
		/* positive hard limit means abort */
		} else if ( limit->lms_t_hard > 0 ) {
			send_search_result( conn, op, LDAP_UNWILLING_TO_PERFORM,
					NULL, NULL, NULL, NULL, 0 );
			rc = 0;
			goto finish;
		}
		
		/* negative hard limit means no limit */
	}
	
	/* if no size limit requested, rely on remote server limits */
	/* if requested limit higher than hard limit, abort */
	if ( !isroot && slimit > limit->lms_s_hard ) {
		/* no hard limit means use soft instead */
		if ( limit->lms_s_hard == 0 ) {
			slimit = limit->lms_s_soft;
			
		/* positive hard limit means abort */
		} else if ( limit->lms_s_hard > 0 ) {
			send_search_result( conn, op, LDAP_UNWILLING_TO_PERFORM,
					NULL, NULL, NULL, NULL, 0 );
			rc = 0;
			goto finish;
		}
		
		/* negative hard limit means no limit */
	}

	if (deref != -1)
		ldap_set_option( lc->ld, LDAP_OPT_DEREF, (void *)&deref);
	if (tlimit != -1)
		ldap_set_option( lc->ld, LDAP_OPT_TIMELIMIT, (void *)&tlimit);
	if (slimit != -1)
		ldap_set_option( lc->ld, LDAP_OPT_SIZELIMIT, (void *)&slimit);
	
	if ( !ldap_back_dobind( lc, op ) ) {
		return( -1 );
	}

	/*
	 * Rewrite the search base, if required
	 */
#ifdef ENABLE_REWRITE
 	switch ( rewrite_session( li->rwinfo, "searchBase",
 				base, conn, &mbase ) ) {
	case REWRITE_REGEXEC_OK:
		if ( mbase == NULL ) {
			mbase = ( char * )base;
		}
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
				"[rw] searchBase: \"%s\" -> \"%s\"\n%",
				base, mbase ));
#else /* !NEW_LOGGING */
		Debug( LDAP_DEBUG_ARGS, "rw> searchBase: \"%s\" -> \"%s\"\n%s",
				base, mbase, "" );
#endif /* !NEW_LOGGING */
		break;
		
	case REWRITE_REGEXEC_UNWILLING:
		send_ldap_result( conn, op, LDAP_UNWILLING_TO_PERFORM,
				NULL, "Unwilling to perform", NULL, NULL );
		rc = -1;
		goto finish;

	case REWRITE_REGEXEC_ERR:
		send_ldap_result( conn, op, LDAP_OPERATIONS_ERROR,
				NULL, "Operations error", NULL, NULL );
		rc = -1;
		goto finish;
	}
	
	/*
	 * Rewrite the search filter, if required
	 */
	switch ( rewrite_session( li->rwinfo, "searchFilter",
				filterstr, conn, &mfilter ) ) {
	case REWRITE_REGEXEC_OK:
		if ( mfilter == NULL || mfilter[0] == '\0') {
			if ( mfilter != NULL ) {
				free( mfilter );
			}
			mfilter = ( char * )filterstr;
		}
#ifdef NEW_LOGGING
		LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
				"[rw] searchFilter: \"%s\" -> \"%s\"\n",
				filterstr, mfilter ));
#else /* !NEW_LOGGING */
		Debug( LDAP_DEBUG_ARGS,
				"rw> searchFilter: \"%s\" -> \"%s\"\n%s",
				filterstr, mfilter, "" );
#endif /* !NEW_LOGGING */
		break;
		
	case REWRITE_REGEXEC_UNWILLING:
		send_ldap_result( conn, op, LDAP_UNWILLING_TO_PERFORM,
				NULL, "Unwilling to perform", NULL, NULL );
	case REWRITE_REGEXEC_ERR:
		rc = -1;
		goto finish;
	}
#else /* !ENABLE_REWRITE */
	mbase = ldap_back_dn_massage( li, ch_strdup( base ), 0 );
#endif /* !ENABLE_REWRITE */

	mapped_filter = ldap_back_map_filter(&li->at_map, &li->oc_map,
#ifdef ENABLE_REWRITE
			(char *)mfilter,
#else /* !ENABLE_REWRITE */
			(char *)filterstr,
#endif /* !ENABLE_REWRITE */
		       	0);
	if ( mapped_filter == NULL ) {
#ifdef ENABLE_REWRITE
		mapped_filter = (char *)mfilter;
#else /* !ENABLE_REWRITE */
		mapped_filter = (char *)filterstr;
#endif /* !ENABLE_REWRITE */
	}

	mapped_attrs = ldap_back_map_attrs(&li->at_map, attrs, 0);
	if ( mapped_attrs == NULL ) {
		mapped_attrs = attrs;
	}

	if ((msgid = ldap_search(lc->ld, mbase, scope, mapped_filter, mapped_attrs,
		attrsonly)) == -1)
	{
fail:;
		rc = ldap_back_op_result(lc, op);
		goto finish;
	}

	/* We pull apart the ber result, stuff it into a slapd entry, and
	 * let send_search_entry stuff it back into ber format. Slow & ugly,
	 * but this is necessary for version matching, and for ACL processing.
	 */
	
	for (	count=0, rc=0;
			rc != -1;
			rc = ldap_result(lc->ld, LDAP_RES_ANY, 0, &tv, &res))
	{
		int ab;

		/* check for abandon */
		ldap_pvt_thread_mutex_lock( &op->o_abandonmutex );
		ab = op->o_abandon;
		ldap_pvt_thread_mutex_unlock( &op->o_abandonmutex );

		if (ab) {
			ldap_abandon(lc->ld, msgid);
			rc = 0;
			goto finish;
		}
		if (rc == 0) {
			tv.tv_sec = 0;
			tv.tv_usec = 100000;
			ldap_pvt_thread_yield();
		} else if (rc == LDAP_RES_SEARCH_ENTRY) {
			e = ldap_first_entry(lc->ld,res);
			ldap_send_entry(be, op, lc, e, attrs, attrsonly);
			count++;
			ldap_msgfree(res);
		} else {
			sres = ldap_result2error(lc->ld, res, 1);
			sres = ldap_back_map_result(sres);
			ldap_get_option(lc->ld, LDAP_OPT_ERROR_STRING, &err);
			ldap_get_option(lc->ld, LDAP_OPT_MATCHED_DN, &match);
			rc = 0;
			break;
		}
	}

	if (rc == -1)
		goto fail;

#ifdef ENABLE_REWRITE
	/*
	 * Rewrite the matched portion of the search base, if required
	 */
	if ( match != NULL ) {
		switch ( rewrite_session( li->rwinfo, "matchedDn",
				match, conn, &mmatch ) ) {
		case REWRITE_REGEXEC_OK:
			if ( mmatch == NULL ) {
				mmatch = ( char * )match;
			}
#ifdef NEW_LOGGING
			LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
					"[rw]  matchedDn:"
					" \"%s\" -> \"%s\"\n",
					match, mmatch ));
#else /* !NEW_LOGGING */
			Debug( LDAP_DEBUG_ARGS, "rw> matchedDn:"
					" \"%s\" -> \"%s\"\n%s",
					match, mmatch, "" );
#endif /* !NEW_LOGGING */
			break;
			
		case REWRITE_REGEXEC_UNWILLING:
			send_ldap_result( conn, op, LDAP_UNWILLING_TO_PERFORM,
					NULL, "Unwilling to perform",
				       	NULL, NULL );
			
		case REWRITE_REGEXEC_ERR:
			rc = -1;
			goto finish;
		}
	}

	send_search_result( conn, op, sres,
		mmatch, err, NULL, NULL, count );

#else /* !ENABLE_REWRITE */
	send_search_result( conn, op, sres,
		match, err, NULL, NULL, count );
#endif /* !ENABLE_REWRITE */

finish:;
	if ( match ) {
#ifdef ENABLE_REWRITE
		if ( mmatch != match ) {
			free( mmatch );
		}
#endif /* ENABLE_REWRITE */
		free(match);
	}
	if ( err ) {
		free( err );
	}
	if ( mapped_attrs != attrs ) {
		charray_free( mapped_attrs );
	}
#ifdef ENABLE_REWRITE
	if ( mapped_filter != mfilter ) {
		free( mapped_filter );
	}
	if ( mfilter != filterstr ) {
		free( mfilter );
	}
#else /* !ENABLE_REWRITE */
	if ( mapped_filter != filterstr ) {
		free( mapped_filter );
	}
#endif /* !ENABLE_REWRITE */
	
#ifdef ENABLE_REWRITE
	if ( mbase != base ) {
#endif /* ENABLE_REWRITE */
		free( mbase );
#ifdef ENABLE_REWRITE
	}
#endif /* ENABLE_REWRITE */
	
	return rc;
}

static void
ldap_send_entry(
	Backend *be,
	Operation *op,
	struct ldapconn *lc,
	LDAPMessage *e,
	struct berval **attrs,
	int attrsonly
)
{
	struct ldapinfo *li = (struct ldapinfo *) be->be_private;
	char *a, *mapped;
	Entry ent;
	BerElement *ber = NULL;
	Attribute *attr, **attrp;
	struct berval *dummy = NULL;
	struct berval *bv;
	const char *text;

#ifdef ENABLE_REWRITE
	char *dn;

	dn = ldap_get_dn(lc->ld, e);
	if ( dn == NULL ) {
		return;
	}

	/*
	 * Rewrite the dn of the result, if needed
	 */
	switch ( rewrite_session( li->rwinfo, "searchResult",
				dn, lc->conn, &ent.e_dn ) ) {
	case REWRITE_REGEXEC_OK:
		if ( ent.e_dn == NULL ) {
			ent.e_dn = dn;
		} else {
#ifdef NEW_LOGGING
			LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
					"[rw] searchResult: \"%s\""
					" -> \"%s\"\n", dn, ent.e_dn ));
#else /* !NEW_LOGGING */
			Debug( LDAP_DEBUG_ARGS, "rw> searchResult: \"%s\""
 					" -> \"%s\"\n%s", dn, ent.e_dn, "" );
#endif /* !NEW_LOGGING */
			free( dn );
			dn = NULL;
		}
		break;
		
	case REWRITE_REGEXEC_ERR:
	case REWRITE_REGEXEC_UNWILLING:
		free( dn );
		return;
	}
#else /* !ENABLE_REWRITE */
	ent.e_dn = ldap_back_dn_restore( li, ldap_get_dn(lc->ld, e), 0 );
#endif /* !ENABLE_REWRITE */

	ent.e_ndn = ch_strdup( ent.e_dn );
	(void) dn_normalize( ent.e_ndn );
	ent.e_id = 0;
	ent.e_attrs = 0;
	ent.e_private = 0;
	attrp = &ent.e_attrs;

	for (	a = ldap_first_attribute(lc->ld, e, &ber);
			a != NULL;
			a = ldap_next_attribute(lc->ld, e, ber))
	{
		mapped = ldap_back_map(&li->at_map, a, 1);
		if (mapped == NULL)
			continue;
		attr = (Attribute *)ch_malloc( sizeof(Attribute) );
		if (attr == NULL)
			continue;
		attr->a_next = 0;
		attr->a_desc = NULL;
		if (slap_str2ad(mapped, &attr->a_desc, &text) != LDAP_SUCCESS) {
			if (slap_str2undef_ad(mapped, &attr->a_desc, &text) 
					!= LDAP_SUCCESS) {
#ifdef NEW_LOGGING
				LDAP_LOG(( "backend", LDAP_LEVEL_DETAIL1,
						"slap_str2undef_ad(%s):	"
						"%s\n", mapped, text ));
#else /* !NEW_LOGGING */
				Debug( LDAP_DEBUG_ANY, 
						"slap_str2undef_ad(%s):	"
 						"%s\n%s", mapped, text, "" );
#endif /* !NEW_LOGGING */
				
				ch_free(attr);
				continue;
			}
		}
		attr->a_vals = ldap_get_values_len(lc->ld, e, a);
		if (!attr->a_vals) {
			attr->a_vals = &dummy;
		} else if ( strcasecmp( mapped, "objectclass" ) == 0 ) {
			int i, last;
			for ( last = 0; attr->a_vals[last]; last++ ) ;
			for ( i = 0; ( bv = attr->a_vals[i] ); i++ ) {
				mapped = ldap_back_map(&li->oc_map, bv->bv_val, 1);
				if (mapped == NULL) {
					ber_bvfree(attr->a_vals[i]);
					attr->a_vals[i] = NULL;
					if (--last < 0)
						break;
					attr->a_vals[i] = attr->a_vals[last];
					attr->a_vals[last] = NULL;
					i--;
				} else if ( mapped != bv->bv_val ) {
					ch_free(bv->bv_val);
					bv->bv_val = ch_strdup( mapped );
					bv->bv_len = strlen( mapped );
				}
			}

#ifdef ENABLE_REWRITE
		/*
		 * It is necessary to try to rewrite attributes with
		 * dn syntax because they might be used in ACLs as
		 * members of groups; since ACLs are applied to the
		 * rewritten stuff, no dn-based subecj clause could
		 * be used at the ldap backend side (see
		 * http://www.OpenLDAP.org/faq/data/cache/452.html)
		 * The problem can be overcome by moving the dn-based
		 * ACLs to the target directory server, and letting
		 * everything pass thru the ldap backend.
		 */
		} else if ( strcmp( attr->a_desc->ad_type->sat_syntax->ssyn_oid,
					SLAPD_DN_SYNTAX ) == 0 ) {
			int i;
			for ( i = 0; ( bv = attr->a_vals[ i ] ); i++ ) {
				char *newval;
				
				switch ( rewrite_session( li->rwinfo,
							"searchResult",
							bv->bv_val,
							lc->conn, &newval )) {
				case REWRITE_REGEXEC_OK:
					/* left as is */
					if ( newval == NULL ) {
						break;
					}
#ifdef NEW_LOGGING
					LDAP_LOG(( "backend",
							LDAP_LEVEL_DETAIL1,
							"[rw] searchResult on"
							" attr=%s:"
							" \"%s\" -> \"%s\"\n",
							attr->a_desc->ad_type->sat_cname.bv_val,
							bv->bv_val, newval ));
#else /* !NEW_LOGGING */
					Debug( LDAP_DEBUG_ARGS,
		"rw> searchResult on attr=%s: \"%s\" -> \"%s\"\n",
						attr->a_desc->ad_type->sat_cname.bv_val,
						bv->bv_val, newval );
#endif /* !NEW_LOGGING */
					
					free( bv->bv_val );
					bv->bv_val = newval;
					bv->bv_len = strlen( newval );
					
					break;
					
				case REWRITE_REGEXEC_UNWILLING:
					
				case REWRITE_REGEXEC_ERR:
					/*
					 * FIXME: better give up,
					 * skip the attribute
					 * or leave it untouched?
					 */
					break;
				}
			}
#endif /* ENABLE_REWRITE */
		}

		*attrp = attr;
		attrp = &attr->a_next;
	}
	send_search_entry( be, lc->conn, op, &ent, attrs, attrsonly, NULL );
	while (ent.e_attrs) {
		attr = ent.e_attrs;
		ent.e_attrs = attr->a_next;
		if (attr->a_vals != &dummy)
			ber_bvecfree(attr->a_vals);
		free(attr);
	}
	if (ber)
		ber_free(ber,0);
	
	if ( ent.e_dn )
		free( ent.e_dn );
	if ( ent.e_ndn )
		free( ent.e_ndn );
}

