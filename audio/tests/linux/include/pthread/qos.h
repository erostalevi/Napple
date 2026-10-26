/* Linux stand-in for <pthread/qos.h> (no QoS classes on Linux). */
#ifndef NAPPLE_LINUX_QOS_H
#define NAPPLE_LINUX_QOS_H
#define QOS_CLASS_USER_INTERACTIVE 0x21
static inline int pthread_set_qos_class_self_np(int c, int p) {
    (void)c;
    (void)p;
    return 0;
}
#endif
