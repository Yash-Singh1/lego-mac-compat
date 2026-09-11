#ifndef LP32_FOCUS_POLICY_H
#define LP32_FOCUS_POLICY_H
/* Environment overrides the bundle setting; absence of both means off. */
int lp32_focus_setting(const char *environment, int bundle_default);
int lp32_continue_when_inactive(void);
int lp32_ignore_guest_focus_loss(void);
int lp32_suppress_background_input(void);
/* Carbon presenters opt in and report WillResign/DidBecomeActive immediately. */
void lp32_set_managed_input_active(int active);
#endif
