-- Runs: pairs bib numbers with start and finish trips.
-- Run this after schema.sql: SQL Editor > New query > paste > Run.
-- Safe to run again.
--
-- How pairing works (all in the database, so it works even with no app open):
--   1. The starter arms a bib in the app. That adds a run with status 'armed'.
--   2. The next start trip goes to the oldest armed run -> 'on_course'.
--   3. The next finish trip goes to the on-course run that started first -> 'finished'.
--   A trip with no run waiting stays unassigned; fix it in the app's Fix tab.

create table if not exists runs (
  id              bigint generated always as identity primary key,
  bib             text not null,
  armed_at        timestamptz not null default now(),
  start_trip_id   bigint unique references trips(id),
  finish_trip_id  bigint unique references trips(id),
  status          text not null default 'armed'
                  check (status in ('armed', 'on_course', 'finished', 'dnf', 'cancelled')),
  note            text
);

create index if not exists runs_status_idx on runs (status, armed_at);

-- Pair each new trip with a run.
create or replace function pair_trip() returns trigger
language plpgsql security definer set search_path = public as $$
declare
  target bigint;
begin
  if new.role = 'start' then
    select id into target from runs
      where status = 'armed' and start_trip_id is null
      order by armed_at, id
      limit 1
      for update skip locked;
    if target is not null then
      update runs set start_trip_id = new.id, status = 'on_course' where id = target;
    end if;

  elsif new.role = 'finish' then
    select r.id into target from runs r
      join trips s on s.id = r.start_trip_id
      where r.status = 'on_course' and r.finish_trip_id is null
        and (new.utc_us = 0 or s.utc_us = 0 or s.utc_us < new.utc_us)
      order by s.utc_us, r.id
      limit 1
      for update of r skip locked;
    if target is not null then
      update runs set finish_trip_id = new.id, status = 'finished' where id = target;
    end if;
  end if;
  return new;
end;
$$;

drop trigger if exists trips_pair on trips;
create trigger trips_pair after insert on trips
  for each row execute function pair_trip();

-- Fixes from the app ---------------------------------------------------------

-- Swap the finishes of two runs (rider B passed rider A on course).
create or replace function swap_finish(run_a bigint, run_b bigint) returns void
language plpgsql security definer set search_path = public as $$
declare
  fa bigint;
  fb bigint;
begin
  select finish_trip_id into fa from runs where id = run_a for update;
  select finish_trip_id into fb from runs where id = run_b for update;
  update runs set finish_trip_id = null where id in (run_a, run_b);
  update runs set finish_trip_id = fb,
    status = case when fb is null then 'on_course' else 'finished' end where id = run_a;
  update runs set finish_trip_id = fa,
    status = case when fa is null then 'on_course' else 'finished' end where id = run_b;
end;
$$;

-- Attach an unassigned trip to a run (or move it from another run).
create or replace function assign_trip(p_run bigint, p_trip bigint) returns void
language plpgsql security definer set search_path = public as $$
declare
  trip_role text;
begin
  select role into trip_role from trips where id = p_trip;
  if trip_role is null then
    raise exception 'trip % not found', p_trip;
  end if;
  if trip_role = 'start' then
    update runs set start_trip_id = null,
      status = case when status = 'on_course' then 'armed' else status end
      where start_trip_id = p_trip and id <> p_run;
    update runs set start_trip_id = p_trip,
      status = case when finish_trip_id is null then 'on_course' else 'finished' end
      where id = p_run;
  else
    update runs set finish_trip_id = null,
      status = case when status = 'finished' then 'on_course' else status end
      where finish_trip_id = p_trip and id <> p_run;
    update runs set finish_trip_id = p_trip, status = 'finished' where id = p_run;
  end if;
end;
$$;

-- Results view ------------------------------------------------------------------
create or replace view run_results with (security_invoker = true) as
select
  r.id, r.bib, r.status, r.armed_at, r.note,
  r.start_trip_id, r.finish_trip_id,
  s.utc_us as start_utc_us,
  f.utc_us as finish_utc_us,
  case when s.utc_us > 0 and f.utc_us > 0 then f.utc_us - s.utc_us end as elapsed_us,
  (coalesce(s.synced, false) and coalesce(f.synced, false)) as synced
from runs r
left join trips s on s.id = r.start_trip_id
left join trips f on f.id = r.finish_trip_id;

-- Trips that no run has claimed.
create or replace view unassigned_trips with (security_invoker = true) as
select t.* from trips t
where not exists (select 1 from runs r where r.start_trip_id = t.id or r.finish_trip_id = t.id);

-- Permissions: the app uses the publishable (anon) key.
alter table runs enable row level security;
drop policy if exists "anon read runs" on runs;
drop policy if exists "anon insert runs" on runs;
drop policy if exists "anon update runs" on runs;
create policy "anon read runs"   on runs for select to anon using (true);
create policy "anon insert runs" on runs for insert to anon with check (true);
create policy "anon update runs" on runs for update to anon using (true) with check (true);

grant select, insert, update on runs to anon;
grant select on run_results, unassigned_trips to anon;
grant execute on function swap_finish(bigint, bigint) to anon;
grant execute on function assign_trip(bigint, bigint) to anon;

-- Live updates for the app.
do $$
begin
  if not exists (select 1 from pg_publication_tables
                 where pubname = 'supabase_realtime' and tablename = 'runs') then
    alter publication supabase_realtime add table runs;
  end if;
end $$;
