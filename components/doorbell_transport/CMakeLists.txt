# Protocol frames over UDP: dual-path send (I-29, I-33) and the confirmable request / ACK transaction loop shared by
# the panel (RING) and the apartment module (OPEN_DOOR, HEARTBEAT, PAIR_COMPLETE)
idf_component_register(
    SRCS "src/doorbell_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES doorbell_protocol
    PRIV_REQUIRES doorbell_net esp_timer esp_system esp_hw_support lwip
)
