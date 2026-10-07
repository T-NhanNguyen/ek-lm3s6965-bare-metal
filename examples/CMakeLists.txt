# Example applications. Each one shows how a consumer links the BSP.
add_subdirectory(baremetal)
add_subdirectory(freertos)
add_subdirectory(ethernet-link)
add_subdirectory(ethernet-raw)
if(LM3S6965_BUILD_ETHERNET_FTP)
    add_subdirectory(ethernet-ftp)
endif()
