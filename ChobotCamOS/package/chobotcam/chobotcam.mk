################################################################################
# ChobotCam
################################################################################
CHOBOTCAM_VERSION = 2.0
CHOBOTCAM_SITE = $(BR2_EXTERNAL_CHOBOT_PATH)/package/chobotcam/src
CHOBOTCAM_SITE_METHOD = local

define CHOBOTCAM_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -pthread -o $(@D)/chobotcam $(@D)/chobotcam.c $(TARGET_LDFLAGS)
endef

define CHOBOTCAM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/chobotcam $(TARGET_DIR)/usr/bin/chobotcam
endef

$(eval $(generic-package))
