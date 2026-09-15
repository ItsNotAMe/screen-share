if(NOT TARGET ScreenShareRoom)
    find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets)
    add_library(ScreenShareRoom STATIC
        "${CMAKE_CURRENT_LIST_DIR}/../backend/room/protocol/RoomProtocol.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/room/protocol/StateSubscription.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/room/qt/RoomSocket.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/room/qt/RoomAdmission.cpp")
    target_compile_features(ScreenShareRoom PUBLIC cxx_std_20)
    target_include_directories(ScreenShareRoom PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../backend")
    target_link_libraries(ScreenShareRoom PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets)
endif()
