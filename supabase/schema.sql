-- Race timing database. Paste into Supabase: SQL Editor > New query > Run.

-- One row per beam trip, posted by the boxes.
create table if not exists trips (
  id           bigint generated always as identity primary key,
  box_id       text    not null,            -- e.g. start-a1b2
  role         text    not null check (role in ('start', 'finish')),
  seq          integer not null,            -- per-box counter, survives reboots
  utc_us       bigint  not null,            -- GPS UTC time of the trip, microseconds (0 = no GPS yet)
  sync_age_ms  integer not null,            -- ms since the last GPS anchor (-1 = none)
  synced       boolean not null,            -- false = don't trust utc_us
  local_us     bigint  not null,            -- box's own uptime clock, for debugging
  received_at  timestamptz not null default now(),
  unique (box_id, seq)                      -- re-sent trips are ignored
);

-- Latest heartbeat from each box, so the app can show "GPS locked / online".
create table if not exists box_status (
  box_id         text primary key,
  role           text not null,
  gps_locked     boolean,
  sats           integer,
  anchor_age_ms  integer,
  pending        integer,
  rssi           integer,
  fw             text,
  last_seen      timestamptz not null default now()
);

create or replace function touch_last_seen() returns trigger as $$
begin
  new.last_seen := now();
  return new;
end;
$$ language plpgsql;

drop trigger if exists box_status_touch on box_status;
create trigger box_status_touch before insert or update on box_status
  for each row execute function touch_last_seen();

-- Row level security: the boxes and the app use the public anon key.
-- Anyone with the key can read and add rows, which is fine for a club tool.
alter table trips enable row level security;
alter table box_status enable row level security;

drop policy if exists "anon read trips" on trips;
drop policy if exists "anon insert trips" on trips;
create policy "anon read trips"   on trips for select to anon using (true);
create policy "anon insert trips" on trips for insert to anon with check (true);

drop policy if exists "anon read status" on box_status;
drop policy if exists "anon write status" on box_status;
drop policy if exists "anon update status" on box_status;
create policy "anon read status"   on box_status for select to anon using (true);
create policy "anon write status"  on box_status for insert to anon with check (true);
create policy "anon update status" on box_status for update to anon using (true) with check (true);

grant select, insert on trips to anon;
grant select, insert, update on box_status to anon;

-- Live updates for the results app.
do $$
begin
  if not exists (select 1 from pg_publication_tables
                 where pubname = 'supabase_realtime' and tablename = 'trips') then
    alter publication supabase_realtime add table trips;
  end if;
  if not exists (select 1 from pg_publication_tables
                 where pubname = 'supabase_realtime' and tablename = 'box_status') then
    alter publication supabase_realtime add table box_status;
  end if;
end $$;
