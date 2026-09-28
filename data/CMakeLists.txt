
# ayatana-indicator-a11y.service

pkg_check_modules (SYSTEMD systemd)

if (${SYSTEMD_FOUND})

    pkg_get_variable (SYSTEMD_USER_DIR systemd systemduserunitdir)
    configure_file ("${CMAKE_CURRENT_SOURCE_DIR}/${CMAKE_PROJECT_NAME}.service.in" "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.service")
    install (FILES "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.service" DESTINATION "${SYSTEMD_USER_DIR}")

endif()

# ayatana-indicator-a11y.desktop

configure_file ("${CMAKE_CURRENT_SOURCE_DIR}/${CMAKE_PROJECT_NAME}.desktop.in" "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.desktop")
install (FILES "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_PROJECT_NAME}.desktop" DESTINATION "/etc/xdg/autostart")

# org.ayatana.indicator.a11y

install (FILES "${CMAKE_CURRENT_SOURCE_DIR}/org.ayatana.indicator.a11y" DESTINATION "${CMAKE_INSTALL_FULL_DATADIR}/ayatana/indicators")

# org.ayatana.indicator.a11y.AccountsService.xml

install (FILES org.ayatana.indicator.a11y.AccountsService.xml DESTINATION "${CMAKE_INSTALL_FULL_DATADIR}/dbus-1/interfaces")
install (DIRECTORY DESTINATION "${CMAKE_INSTALL_FULL_DATADIR}/accountsservice/interfaces/")
install (CODE "message(STATUS \"Symlinking: \$ENV{DESTDIR}${CMAKE_INSTALL_FULL_DATADIR}/accountsservice/interfaces/org.ayatana.indicator.a11y.AccountsService.xml\")")
install (CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ../../dbus-1/interfaces/org.ayatana.indicator.a11y.AccountsService.xml \$ENV{DESTDIR}${CMAKE_INSTALL_FULL_DATADIR}/accountsservice/interfaces/org.ayatana.indicator.a11y.AccountsService.xml)")

# org.ayatana.indicator.a11y.AccountsService.policy

install (FILES org.ayatana.indicator.a11y.AccountsService.policy DESTINATION "${CMAKE_INSTALL_FULL_DATADIR}/polkit-1/actions")

# 50-org.ayatana.indicator.a11y.AccountsService.rules

install (FILES 50-org.ayatana.indicator.a11y.AccountsService.rules DESTINATION "${CMAKE_INSTALL_FULL_DATADIR}/polkit-1/rules.d")

# org.ayatana.indicator.a11y.gschema.xml

find_package (GSettings REQUIRED)
add_schema ("org.ayatana.indicator.a11y.gschema.xml")

# ayatana-indicator-a11y-scale-down.svg
# ayatana-indicator-a11y-scale-up.svg

install (DIRECTORY "icons/" DESTINATION "${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/scalable/status" FILES_MATCHING PATTERN "*.svg")
