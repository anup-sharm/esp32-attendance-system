idf_component_register(
    SRCS
        "main.c"

        "database_reset.c"

        "wifi/wifi_manager.c"

        "camera/camera.c"

        "display/tft.c"

        "face_detection/face_detection.cpp"

        "face_recognition/face_recognition.cpp"

        "storage/storage.cpp"
        "storage/csv.c"
        "storage/sdcard.c"

        "rtc/ds3231.c"

        "employee/employee_db.c"

        "attendance/attendance.c"

        "reports/reports.c"

        "security/hash_chain.c"

        "web/web_server.c"

        "audio/audio.cpp"

    INCLUDE_DIRS
        "."
        "wifi"
        "camera"
        "display"
        "face"
        "face_detection"
        "face_recognition"
        "storage"
        "rtc"
        "employee"
        "attendance"
        "reports"
        "security"
        "web"
        "audio"

    REQUIRES
        esp_driver_i2c
        esp_driver_uart
        esp_driver_i2s

        driver

        esp_event
        esp_netif
        esp_wifi
        nvs_flash

        esp_http_server
        esp_timer
        esp_partition

        fatfs
        vfs

        human_face_detect
        human_face_recognition

        spiflash_fatfs
        esp-dl

        picotts
)