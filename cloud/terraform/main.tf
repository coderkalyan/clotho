terraform {
  required_version = ">= 1.6"
  required_providers {
    latitudesh = {
      source  = "latitudesh/latitudesh"
      version = ">= 2.5.0"
    }
  }
}

# Token from env LATITUDESH_AUTH_TOKEN (do not put it in tfvars/state).
provider "latitudesh" {}

# cloud-init: box-setup only (deps + quiet knobs). Deliberately secret-free —
# the self-delete watchdog needs the server id (known only post-create) and a
# token, so run.sh arms it over SSH after boot, keeping secrets out of state.
resource "latitudesh_user_data" "boxsetup" {
  project     = var.project
  description = "clotho-bench box-setup (deps + quiet hardware)"
  content = base64encode(templatefile("${path.module}/../cloud-init.tftpl", {
    box_setup_b64 = base64encode(file("${path.module}/../box-setup.sh"))
  }))
}

resource "latitudesh_server" "bench" {
  hostname         = var.hostname
  operating_system = var.operating_system
  plan             = var.plan
  site             = var.site
  project          = var.project
  billing          = "hourly"
  ssh_keys         = var.ssh_key_ids
  user_data        = latitudesh_user_data.boxsetup.id
}
