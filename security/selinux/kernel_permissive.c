/** License GPL-v2
    Copyright 2020 by Pal Zoltan Illes
*/

//#define DEBUG_K_PERM
static bool kernel_permissive_check(struct selinux_state *state, u32 ssid, u32 tsid, u16 tclass) {
        int rc1,rc2;
        char *scontext;
        char *tcontext;
        u32 scontext_len;
        bool permissive = false;

        if (!kernel_permissive) return false;

        if (state==NULL) return false;

        rc1 = security_sid_to_context(state, ssid, &scontext, &scontext_len);
        if (!rc1 && !strcmp(scontext, KERNEL_SOURCE) ) {
                int i;
                rc2 = security_sid_to_context(state, tsid, &tcontext, &scontext_len);
                if (!rc2) {
#ifdef DEBUG_K_PERM
                        pr_err("%s kernel permissive scontext match %s - checking in list for tcontext: %s \n",__func__,scontext,tcontext);
#endif
                        for (i=0;i<TARGETS_LENGTH;i++) {
                                if (!strcmp(targets[i],tcontext)) {
                                        pr_err("%s kernel permissive scontext / tcontext match %s / %s . Setting permissive.. [userland]\n",__func__,scontext,tcontext);
                                        permissive = true;
                                        break;
                                }
                        }
                }
                kfree(tcontext);
        }
#ifdef DEBUG_K_PERM
	else {
                rc2 = security_sid_to_context(state, tsid, &tcontext, &scontext_len);
                if (!rc1 && !rc2) {
                        pr_err("%s kernel permissive scontext NO match | scontext: %s - tcontext: %s \n",__func__,scontext,tcontext);
                } else {
                        pr_err("%s kernel permissive scontext NO match | sid: %s - tid: %s \n",__func__,ssid,tsid);
                }
                if (!rc2) kfree(tcontext);
        }
#endif
        kfree(scontext);
        return permissive;
}
