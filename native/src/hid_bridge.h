#ifndef LP32_HID_BRIDGE_H
#define LP32_HID_BRIDGE_H
#include <stdint.h>
int hid_bridge32_dispatch(const char *,const uint32_t *,uint64_t *);
int hid_bridge32_test_button(unsigned,int);
int hid_bridge32_test_named_button(const char *,unsigned,int);
int hid_bridge32_test_hat(unsigned);
int hid_bridge32_run_ds4_map_self_test(void);
/* Saga presents controllers with no publisher profile as a canonical gamepad.
   The host controller API supplies semantic controls for paired controllers. */
unsigned hid_bridge32_saga_pad_count(void);
const char *hid_bridge32_saga_pad_name(unsigned index);
void hid_bridge32_saga_set_semantic_active(unsigned index,int active);
int hid_bridge32_saga_emit(unsigned index,unsigned page,unsigned usage,int value);
#endif
