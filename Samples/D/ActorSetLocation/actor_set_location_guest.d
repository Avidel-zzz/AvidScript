module actor_set_location_guest;

extern(C) @nogc nothrow
{
    int actor_set_location(int slot, int generation, float x, float y, float z);

    export void avid_on_begin_play()
    {
        cast(void)actor_set_location(1, 1, 123.0f, 456.0f, 789.0f);
    }

    export void avid_on_tick(float delta_seconds)
    {
        cast(void)delta_seconds;
    }
}
