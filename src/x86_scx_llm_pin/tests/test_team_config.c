/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>
#include "team_config.h"
int main(void)
{
    struct llm_pin_config cfg = {0};
    struct llm_team_config t;
    const char *bad[] = {"1:0:8", "1:9:8", "1:8:32", "0:4:4", "1:4:4:0x3",
        "1:1:1:0x100", "1:1", "-1:2:2", "1:2:2:", "1:2:2:0xff:x", "1:2x:2"};
    for (unsigned i=0; i<sizeof(bad)/sizeof(*bad); i++) assert(llm_parse_team(bad[i], &t));
    assert(!llm_parse_team("100:4:4", &cfg.teams[0]));
    assert(!llm_parse_team("101:2:2", &cfg.teams[1]));
    assert(!llm_parse_team("102:2:2", &cfg.teams[2]));
    cfg.team_count=3;
    assert(!llm_prepare_teams(&cfg));
    unsigned used=0;
    for (unsigned team=0; team<3; team++)
        for (unsigned i=0; i<cfg.teams[team].threads; i++) {
            unsigned cpu=cfg.teams[team].cpu_order[i];
            assert(!(used & (1U<<cpu))); used |= 1U<<cpu;
        }
    assert(used == 0xff);
    assert(!llm_parse_team("103:1:4:0x55", &t));
    cfg.teams[2].tgid=cfg.teams[0].tgid;
    assert(llm_prepare_teams(&cfg));
    cfg.team_count=RVV_LEASE_CONTROL_TEAMS+1;
    assert(llm_prepare_teams(&cfg));
    assert(sizeof(struct rvv_lease_worker_page)==4096);
    puts("team configuration tests passed");
}
