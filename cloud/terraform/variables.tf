variable "project" {
  type        = string
  description = "Latitude.sh project ID or slug (proj_...)."
}

variable "hostname" {
  type    = string
  default = "clotho-bench"
}

variable "operating_system" {
  type        = string
  default     = "ubuntu_24_04_x64_lts"
  description = "24.04 clears the static binary's glibc>=2.38 floor."
}

variable "plan" {
  type        = string
  description = "Server plan slug (e.g. c3-large-x86). Pick a quiet single-socket x86 plan."
}

variable "site" {
  type        = string
  description = "Site slug (e.g. ASH, CHI, FRA, TYO4). Pick one where the plan is in stock."
}

variable "ssh_key_ids" {
  type        = list(string)
  description = "Latitude SSH key IDs (ssh_...) authorized for login (see terraform.tfvars.example)."
}
