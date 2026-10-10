{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* polyhook binding slots (defined by polyhook_bindings, filled by the plugify module
           from the polyhook plugin's exported methods) */
        __polyhook_*;

        /* hudmenu binding slots: pisexcompat PUBLIC-links hudmenu_bindings, so every consumer
           exports them (and declares the optional hudmenu dependency) */
        __hudmenu_*;

    local: *;
};
