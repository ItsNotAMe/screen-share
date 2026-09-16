if(NOT TARGET ScreenShareRoomSession)
    add_library(ScreenShareRoomSession STATIC
        "${CMAKE_CURRENT_LIST_DIR}/../backend/room/qt/RoomMediaSession.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/room/qt/RoomSignalCodec.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/room/qt/RoomSessionCoordinator.cpp")
    target_compile_features(ScreenShareRoomSession PUBLIC cxx_std_20)
    target_compile_options(ScreenShareRoomSession PRIVATE /EHsc /GR)
    target_link_libraries(ScreenShareRoomSession PUBLIC ScreenShareNegotiation ScreenShareRoom)
endif()
